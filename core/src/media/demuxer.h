#ifndef LYCHEE_MEDIA_DEMUXER_H_
#define LYCHEE_MEDIA_DEMUXER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "media/encoded_packet.h"
#include "media/stream_info.h"

namespace lychee {

enum class DemuxerReadResult { kOk, kEndOfStream, kError, kAborted };

// A variant of an adaptive stream (an HLS master playlist's).
struct VariantInfo {
  // Declared bit/s, picture size and frame rate (0 if not declared), and
  // codecs (HLS's CODECS attribute, else the codec's name).
  int64_t bandwidth = 0;
  int width = 0;
  int height = 0;
  double frame_rate = 0;
  std::string codecs;
  // The video stream that plays it.
  int stream = -1;
  // The demuxer moves |stream| between the adaptive variants by itself
  // (SetVariantPolicy()); the others play only on their own stream.
  bool adaptive = false;
};

// The part of a live input's timeline that a seek reaches, in positions
// (media time less MediaInfo::start_time).
struct LiveWindow {
  int64_t start = 0;
  int64_t end = 0;
  // Where playback starts at the open: far enough from |end| that the
  // input keeps ahead (HLS: three target durations).
  int64_t edge = 0;
  // MonotonicMicros() of the playlist these come from. The window moves on
  // in real time from then (its start too if |sliding|) while |moving|.
  int64_t updated = 0;
  bool sliding = false;
  bool moving = false;
};

class Demuxer {
 public:
  virtual ~Demuxer() = default;

  virtual const char* name() const = 0;

  // Blocking. Fills media_info() on success.
  virtual bool Open(const std::string& url, std::string* error) = 0;

  virtual const MediaInfo& media_info() const = 0;

  // Only packets of the enabled streams are returned.
  virtual void SetEnabledStreams(const std::vector<int>& stream_indices) = 0;

  // Blocking. kError means the data cannot be read to its end (network
  // failure, truncated file); error() says why.
  virtual DemuxerReadResult Read(EncodedPacket* packet) = 0;

  // Seeks to the closest keyframe at or before |time| (media timeline).
  // On input that cannot seek it may fail without moving the read position.
  virtual bool Seek(int64_t time) = 0;

  // Thread-safe. Makes the current blocking Read() or Seek() return early
  // (kAborted / false), e.g. so a new seek does not wait for a stalled
  // network read. Cleared by the next Seek().
  virtual void Interrupt() = 0;

  // Thread-safe. Makes pending and future blocking calls return early.
  virtual void Abort() = 0;

  // Why the last Read() returned kError.
  virtual std::string error() const = 0;

  // Called on the thread that reads. Bits per second the input last
  // arrived at over the network (DataSource::transfer_rate(); HLS: its
  // bandwidth estimate), or -1 if unknown.
  virtual int64_t download_rate() const { return -1; }

  // The variants of an adaptive stream, fixed once Open() returns; empty
  // for other input.
  virtual std::vector<VariantInfo> variants() const { return {}; }

  // Called on the thread that reads. The variant (an index into
  // variants()) the last video packet read came from, or -1.
  virtual int current_variant() const { return -1; }

  // Thread-safe, also before Open(). The adaptive variant to keep to (an
  // index into variants()), or -1 to follow the bandwidth; and the
  // highest bandwidth (bit/s) to move to on its own, 0 for any. Takes
  // effect from the next segment read.
  virtual void SetVariantPolicy(int /*pinned*/, int64_t /*max_bandwidth*/) {}

  // Thread-safe once Open() returned. False unless the input is live and
  // can seek within a window (MediaInfo::seekable).
  virtual bool live_window(LiveWindow* /*window*/) const { return false; }
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_DEMUXER_H_
