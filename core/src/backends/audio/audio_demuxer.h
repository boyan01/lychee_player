#ifndef LYCHEE_BACKENDS_AUDIO_AUDIO_DEMUXER_H_
#define LYCHEE_BACKENDS_AUDIO_AUDIO_DEMUXER_H_

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/data_source.h"
#include "media/demuxer.h"

namespace lychee {

// Shared byte IO only: container parsing stays in each demuxer. The cache
// avoids one HTTP range request per small frame or sync byte.
class AudioFileDemuxer : public Demuxer {
 public:
  bool Open(const std::string& url, std::string* error) override {
    auto source = OpenDataSource(url, error);
    return source && Open(std::move(source), error);
  }
  bool Open(std::unique_ptr<DataSource> source, std::string* error) {
    {
      std::lock_guard lock(source_mutex_);
      source_ = std::move(source);
      if (aborted_) {
        source_->Interrupt();
        *error = "audio input aborted";
        return false;
      }
    }
    file_size_ = source_->size();
    if (file_size_ > INT64_MAX - int64_t{16} * 1024 * 1024) {
      *error = "audio input is too large";
      return false;
    }
    if (!Parse()) {
      *error = error_.empty() ? "invalid audio container" : error_;
      return false;
    }
    media_info_.format_name = name();
    media_info_.seekable = source_->seekable() && file_size_ >= 0;
    media_info_.default_audio_stream = 0;
    stream_.index = 0;
    stream_.type = MediaType::kAudio;
    stream_.default_track = true;
    stream_.duration = media_info_.duration;
    media_info_.streams = {stream_};
    return true;
  }
  const MediaInfo& media_info() const override { return media_info_; }
  void SetEnabledStreams(const std::vector<int>& streams) override {
    enabled_ = std::find(streams.begin(), streams.end(), 0) != streams.end();
  }
  void Interrupt() override {
    interrupted_ = true;
    std::lock_guard lock(source_mutex_);
    if (source_) source_->Interrupt();
  }
  void Abort() override {
    aborted_ = true;
    Interrupt();
  }
  std::string error() const override { return error_; }
  int64_t download_rate() const override {
    return source_ ? source_->transfer_rate() : -1;
  }

 protected:
  virtual bool Parse() = 0;
  bool Cancelled() const { return interrupted_ || aborted_; }
  bool BeginSeek() {
    interrupted_ = false;
    if (aborted_ || !media_info_.seekable) return false;
    source_->ClearInterrupt();
    error_.clear();
    return true;
  }
  // Returns the bytes available; zero is EOF, -1 an IO error/interrupt.
  int64_t Bytes(int64_t offset, uint8_t* out, size_t size) {
    if (offset < 0 || offset > INT64_MAX - int64_t{16} * 1024 * 1024 ||
        size > size_t{16} * 1024 * 1024 || Cancelled())
      return -1;
    size_t done = 0;
    while (done < size) {
      if (cache_.empty() || offset < cache_offset_ ||
          offset >= cache_offset_ + static_cast<int64_t>(cache_.size())) {
        if (file_size_ >= 0 && offset >= file_size_) break;
        cache_.resize(65536);
        const int64_t read =
            source_->Read(offset, cache_.data(), cache_.size());
        if (read < 0) {
          cache_.clear();
          error_ = source_->error();
          return -1;
        }
        cache_.resize(static_cast<size_t>(read));
        cache_offset_ = offset;
        if (read == 0) {
          // A chunked response may have no Content-Length. A real EOF
          // still tells parsers where its final packet ends.
          if (file_size_ < 0) file_size_ = offset;
          break;
        }
      }
      const size_t at = static_cast<size_t>(offset - cache_offset_);
      const size_t n = std::min(size - done, cache_.size() - at);
      memcpy(out + done, cache_.data() + at, n);
      offset += static_cast<int64_t>(n);
      done += n;
      if (Cancelled()) return -1;
    }
    return static_cast<int64_t>(done);
  }
  bool Exact(int64_t offset, uint8_t* out, size_t size) {
    return Bytes(offset, out, size) == static_cast<int64_t>(size);
  }
  bool Packet(int64_t offset,
              size_t size,
              int64_t pts,
              int64_t duration,
              EncodedPacket* packet) {
    std::vector<uint8_t> bytes(size);
    if (!Exact(offset, bytes.data(), size)) return false;
    *packet = {};
    packet->stream_index = 0;
    packet->pts = packet->dts = pts;
    packet->duration = duration;
    packet->keyframe = true;
    packet->buffer = PacketBuffer::Copy(bytes.data(), bytes.size());
    return true;
  }
  DemuxerReadResult Failure() {
    if (Cancelled()) return DemuxerReadResult::kAborted;
    if (error_.empty()) error_ = "truncated or invalid audio packet";
    return DemuxerReadResult::kError;
  }
  static uint32_t BE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | p[3];
  }
  static uint32_t LE32(const uint8_t* p) {
    return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
  }
  static uint16_t LE16(const uint8_t* p) {
    return p[0] | (uint16_t(p[1]) << 8);
  }
  std::mutex source_mutex_;
  std::unique_ptr<DataSource> source_;
  MediaInfo media_info_;
  StreamInfo stream_;
  int64_t file_size_ = -1;
  std::atomic<bool> interrupted_{false};
  std::atomic<bool> aborted_{false};
  bool enabled_ = true;
  std::string error_;
  std::vector<uint8_t> cache_;
  int64_t cache_offset_ = 0;
};

}  // namespace lychee
#endif
