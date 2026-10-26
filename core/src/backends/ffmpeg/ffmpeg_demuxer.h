#ifndef LYCHEE_BACKENDS_FFMPEG_FFMPEG_DEMUXER_H_
#define LYCHEE_BACKENDS_FFMPEG_FFMPEG_DEMUXER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/demuxer.h"

struct AVDictionary;
struct AVFormatContext;
struct AVIOContext;
struct AVPacket;

// On Apple platforms FFmpeg reads files and http(s) through DataSource
// (NSURLSession), as the FFmpeg-free build does. Elsewhere DataSource has
// no http client, so FFmpeg uses its own protocols.
#if defined(__APPLE__)
#define LYCHEE_FFMPEG_DATA_SOURCE 1
#endif

namespace lychee {

class FFmpegDemuxer : public Demuxer {
 public:
  FFmpegDemuxer();
  ~FFmpegDemuxer() override;

  const char* name() const override { return "ffmpeg"; }
  bool Open(const std::string& url, std::string* error) override;
  const MediaInfo& media_info() const override { return media_info_; }
  void SetEnabledStreams(const std::vector<int>& stream_indices) override;
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;
  void Interrupt() override;
  void Abort() override;
  std::string error() const override { return error_; }

 private:
  static int InterruptCallback(void* opaque);
#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
  struct Io;
  static int IoOpen(AVFormatContext* s, AVIOContext** pb, const char* url,
                    int flags, AVDictionary** options);
  static int IoClose(AVFormatContext* s, AVIOContext* pb);
  static int ReadPacket(void* opaque, uint8_t* data, int size);
  static int64_t SeekPacket(void* opaque, int64_t offset, int whence);
  // Null with |error| set if the source cannot be opened or its first
  // bytes cannot be read; |cleartext_refused| then says whether FFmpeg's
  // http should read it instead (DataSource::cleartext_refused()).
  AVIOContext* OpenIo(const std::string& url, std::string* error,
                      bool* cleartext_refused);
  void CloseIo(AVIOContext* pb);
  void RemoveIo(Io* io);
#endif
  void BuildMediaInfo();
  // At end of file: whether the data stopped short of the duration the
  // container declares.
  bool TruncatedAtEnd() const;

  AVFormatContext* format_context_ = nullptr;
  AVPacket* av_packet_ = nullptr;
  std::atomic<bool> aborted_{false};
  std::atomic<bool> interrupted_{false};
  MediaInfo media_info_;
  std::vector<bool> enabled_streams_;
  std::string error_;
  // The duration comes from the container's index rather than an estimate.
  bool declared_duration_ = false;
  // End of the data read since open or the last seek.
  int64_t read_end_ = kNoTimestamp;
#if defined(LYCHEE_FFMPEG_DATA_SOURCE)
  // FFmpeg's own, for URLs DataSource does not read (data:, crypto:).
  int (*default_io_open_)(AVFormatContext*, AVIOContext**, const char*, int,
                          AVDictionary**) = nullptr;
  int (*default_io_close_)(AVFormatContext*, AVIOContext*) = nullptr;
  // Every open source: the input's and what demuxers open themselves (HLS
  // playlists and segments). Interrupt() and Abort() reach them from other
  // threads under the mutex.
  std::mutex ios_mutex_;
  std::vector<std::unique_ptr<Io>> ios_;
  // Why the last open or read failed, from the source.
  std::string io_error_;
#endif
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FFMPEG_FFMPEG_DEMUXER_H_
