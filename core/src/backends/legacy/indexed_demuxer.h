#ifndef LYCHEE_BACKENDS_LEGACY_INDEXED_DEMUXER_H_
#define LYCHEE_BACKENDS_LEGACY_INDEXED_DEMUXER_H_

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "media/data_source.h"
#include "media/demuxer.h"

namespace lychee {

// These containers have no common syntax. Only byte IO and the resulting
// packet index are shared; payloads remain in the DataSource until Read().
class IndexedDemuxer : public Demuxer {
 public:
  bool Open(const std::string& url, std::string* error) override {
    auto source = OpenDataSource(url, error);
    return source && Open(std::move(source), error);
  }
  bool Open(std::unique_ptr<DataSource> source, std::string* error) {
    {
      std::lock_guard lock(source_mutex_);
      source_ = std::move(source);
      if (aborted_) source_->Interrupt();
    }
    if (!Parse() || samples_.empty() || info_.streams.empty()) {
      *error = error_.empty() ? "invalid or unsupported container" : error_;
      return false;
    }
    info_.format_name = name();
    info_.seekable = source_->seekable();
    enabled_.assign(info_.streams.size(), true);
    for (size_t i = 0; i < info_.streams.size(); ++i) {
      auto& stream = info_.streams[i];
      stream.index = static_cast<int>(i);
      if (stream.type == MediaType::kVideo && info_.default_video_stream < 0)
        info_.default_video_stream = stream.index;
      if (stream.type == MediaType::kAudio && info_.default_audio_stream < 0)
        info_.default_audio_stream = stream.index;
    }
    info_.duration = 0;
    for (const auto& sample : samples_)
      info_.duration = std::max(info_.duration, sample.pts + sample.duration);
    info_.duration -= info_.start_time;
    for (auto& stream : info_.streams)
      stream.duration = info_.duration;
    return true;
  }
  const MediaInfo& media_info() const override { return info_; }
  void SetEnabledStreams(const std::vector<int>& streams) override {
    for (size_t i = 0; i < enabled_.size(); ++i)
      enabled_[i] = std::find(streams.begin(), streams.end(),
                              static_cast<int>(i)) != streams.end();
  }
  DemuxerReadResult Read(EncodedPacket* packet) override {
    while (next_ < samples_.size()) {
      if (interrupted_ || aborted_) return DemuxerReadResult::kAborted;
      const Sample& s = samples_[next_++];
      if (!enabled_[s.stream]) continue;
      std::vector<uint8_t> bytes;
      if (!Load(s, &bytes))
        return interrupted_ || aborted_ ? DemuxerReadResult::kAborted
                                        : DemuxerReadResult::kError;
      *packet = {};
      packet->stream_index = s.stream;
      packet->pts = s.pts;
      packet->dts = s.dts;
      packet->duration = s.duration;
      packet->keyframe = s.key;
      packet->buffer = PacketBuffer::Copy(bytes.data(), bytes.size());
      return DemuxerReadResult::kOk;
    }
    return interrupted_ || aborted_ ? DemuxerReadResult::kAborted
                                    : DemuxerReadResult::kEndOfStream;
  }
  bool Seek(int64_t time) override {
    if (aborted_ || !info_.seekable) return false;
    interrupted_ = false;
    source_->ClearInterrupt();
    int anchor = info_.default_video_stream;
    if (anchor < 0 || !enabled_[anchor]) {
      anchor = -1;
      for (size_t i = 0; i < enabled_.size(); ++i) {
        if (enabled_[i]) {
          anchor = static_cast<int>(i);
          break;
        }
      }
    }
    size_t chosen = 0;
    for (size_t i = 0; i < samples_.size(); ++i) {
      const Sample& s = samples_[i];
      if (s.stream == anchor && s.key && s.pts <= time) chosen = i;
    }
    next_ = chosen;
    error_.clear();
    return true;
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
  struct Sample {
    int stream = 0;
    int64_t offset = 0;
    uint32_t size = 0;
    int64_t pts = 0;
    int64_t dts = 0;
    int64_t duration = 0;
    bool key = false;
  };
  virtual bool Parse() = 0;
  virtual bool Load(const Sample& sample, std::vector<uint8_t>* bytes) {
    bytes->resize(sample.size);
    return Exact(sample.offset, bytes->data(), bytes->size());
  }
  bool Exact(int64_t offset, uint8_t* out, size_t size) {
    if (offset < 0 || size > size_t{16} * 1024 * 1024 ||
        offset > INT64_MAX - int64_t{16} * 1024 * 1024)
      return false;
    size_t done = 0;
    while (done < size) {
      if (interrupted_ || aborted_) return false;
      const int64_t at = offset + static_cast<int64_t>(done);
      if (cache_.empty() || at < cache_offset_ ||
          at >= cache_offset_ + static_cast<int64_t>(cache_.size())) {
        cache_.resize(65536);
        const int64_t n = source_->Read(at, cache_.data(), cache_.size());
        if (n <= 0 || n > static_cast<int64_t>(cache_.size())) {
          cache_.clear();
          error_ = n < 0 ? source_->error() : "truncated container";
          return false;
        }
        cache_.resize(static_cast<size_t>(n));
        cache_offset_ = at;
      }
      const size_t within = static_cast<size_t>(at - cache_offset_);
      const size_t n = std::min(size - done, cache_.size() - within);
      memcpy(out + done, cache_.data() + within, n);
      done += n;
    }
    return true;
  }
  bool Add(Sample sample) {
    if (samples_.size() >= 1000000 || sample.size > size_t{16} * 1024 * 1024) {
      error_ = "container index or packet is too large";
      return false;
    }
    samples_.push_back(sample);
    return true;
  }
  static uint32_t BE24(const uint8_t* p) {
    return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
  }
  static uint32_t BE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | BE24(p + 1);
  }
  static uint16_t LE16(const uint8_t* p) { return p[0] | uint16_t(p[1]) << 8; }
  static uint32_t LE32(const uint8_t* p) {
    return LE16(p) | uint32_t(LE16(p + 2)) << 16;
  }
  static uint64_t LE64(const uint8_t* p) {
    return LE32(p) | uint64_t(LE32(p + 4)) << 32;
  }
  std::mutex source_mutex_;
  std::unique_ptr<DataSource> source_;
  std::atomic<bool> interrupted_{false};
  std::atomic<bool> aborted_{false};
  MediaInfo info_;
  std::vector<Sample> samples_;
  std::vector<bool> enabled_;
  size_t next_ = 0;
  std::string error_;
  std::vector<uint8_t> cache_;
  int64_t cache_offset_ = 0;
};
}  // namespace lychee
#endif
