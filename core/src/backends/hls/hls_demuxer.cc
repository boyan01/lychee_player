#define LOG_TAG "hls"

#include "backends/hls/hls_demuxer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "base/log.h"
#include "base/time.h"

namespace lychee {

namespace {

// Playlists are text; one of a day of segments is a few MB.
constexpr size_t kMaxPlaylistBytes = 16 << 20;
// A segment's streams are known once each has a packet; this bounds the
// read when one never has.
constexpr int64_t kSegmentProbeBytes = 4 << 20;
// RFC 8216 6.3.3: start no closer to the end of a live playlist.
constexpr int kLiveStartTargetDurations = 3;
// Failed reloads of a live playlist in a row before giving up.
constexpr int kMaxReloadFailures = 3;
// Segments kept behind the one playing (live).
constexpr size_t kKeptSegments = 64;

}  // namespace

HlsDemuxer::HlsDemuxer() = default;

HlsDemuxer::~HlsDemuxer() {
  CloseSegment();
}

// static
bool HlsDemuxer::Detect(const uint8_t* data, size_t size) {
  if (size >= 3 && memcmp(data, "\xEF\xBB\xBF", 3) == 0) {
    data += 3;
    size -= 3;
  }
  return size >= 7 && memcmp(data, "#EXTM3U", 7) == 0;
}

void HlsDemuxer::Track(DataSource* source) {
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = source;
  if (source && (aborted_ || interrupted_)) {
    source->Interrupt();
  }
}

bool HlsDemuxer::Failed() {
  if (aborted_ || interrupted_) {
    return true;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return active_ && active_->interrupted();
}

bool HlsDemuxer::LoadPlaylist(std::unique_ptr<DataSource> source,
                              const std::string& url,
                              HlsPlaylist* playlist,
                              std::string* final_url,
                              std::string* error) {
  if (!source) {
    source = OpenDataSource(url, error);
    if (!source) {
      return false;
    }
  }
  Track(source.get());
  std::string text;
  uint8_t chunk[16384];
  bool ok = true;
  for (;;) {
    int64_t n = source->Read(static_cast<int64_t>(text.size()), chunk,
                             sizeof(chunk));
    if (n < 0) {
      *error = url + ": " + source->error();
      ok = false;
      break;
    }
    if (n == 0) {
      break;
    }
    text.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n));
    if (text.size() > kMaxPlaylistBytes) {
      *error = url + ": playlist too large";
      ok = false;
      break;
    }
  }
  *final_url = source->url().empty() ? url : source->url();
  Track(segment_source_.get());
  return ok && ParseHlsPlaylist(text, *final_url, playlist, error);
}

bool HlsDemuxer::Open(const std::string& url, std::string* error) {
  return Open(nullptr, url, error);
}

bool HlsDemuxer::Open(std::unique_ptr<DataSource> source,
                      const std::string& url,
                      std::string* error) {
  HlsPlaylist playlist;
  std::string final_url;
  if (!LoadPlaylist(std::move(source), url, &playlist, &final_url, error)) {
    return false;
  }
  playlist_url_ = url;
  if (playlist.master) {
    const int pick = PickHlsVariant(playlist.variants);
    if (pick < 0) {
      *error = playlist.variants.empty()
                   ? "HLS master playlist without variants"
                   : "no HLS variant with H.264, HEVC or AAC";
      return false;
    }
    const HlsVariant& variant = playlist.variants[pick];
    LOGI("variant %d of %zu: %lld bit/s %dx%d %s", pick,
         playlist.variants.size(), static_cast<long long>(variant.bandwidth),
         variant.width, variant.height, variant.codecs.c_str());
    playlist_url_ = variant.url;
    if (!LoadPlaylist(nullptr, playlist_url_, &playlist, &final_url, error)) {
      return false;
    }
    if (playlist.master) {
      *error = "HLS variant is a master playlist";
      return false;
    }
  }
  live_ = !playlist.ended;
  ended_ = playlist.ended;
  target_duration_ = std::max(playlist.target_duration, kMicrosPerSecond);
  Merge(playlist);
  last_reload_ = MonotonicMicros();
  if (segments_.empty() && !live_) {
    *error = "HLS playlist without segments";
    return false;
  }
  if (live_ && !segments_.empty()) {
    // The last segment that starts at least three target durations
    // before the end.
    const Segment& last = segments_.back();
    const int64_t end = last.start + last.duration;
    current_ = 0;
    for (size_t i = segments_.size(); i-- > 0;) {
      if (end - segments_[i].start >=
          kLiveStartTargetDurations * target_duration_) {
        current_ = i;
        break;
      }
    }
  }
  if (live_ && current_ >= segments_.size()) {
    DemuxerReadResult result = WaitForSegments();
    if (result != DemuxerReadResult::kOk) {
      *error = result == DemuxerReadResult::kError ? error_ : "aborted";
      return false;
    }
  }
  if (!OpenSegment(error)) {
    CloseSegment();
    return false;
  }
  media_info_.format_name = "hls";
  media_info_.seekable = !live_;
  if (!live_) {
    const Segment& last = segments_.back();
    media_info_.duration = last.start + last.duration;
  }
  enabled_.assign(media_info_.streams.size(), true);
  need_keyframe_.assign(media_info_.streams.size(), true);
  return true;
}

bool HlsDemuxer::Merge(const HlsPlaylist& playlist) {
  bool added = false;
  for (const HlsSegment& segment : playlist.segments) {
    if (!segments_.empty() && segment.sequence <= segments_.back().sequence) {
      continue;
    }
    Segment entry;
    static_cast<HlsSegment&>(entry) = segment;
    if (!segments_.empty()) {
      entry.start = segments_.back().start + segments_.back().duration;
    }
    segments_.push_back(std::move(entry));
    added = true;
  }
  // Fallen behind a live playlist: what is gone from it cannot be read.
  if (!playlist.segments.empty() && current_ < segments_.size() &&
      segments_[current_].sequence < playlist.segments.front().sequence &&
      !segment_reader_) {
    while (current_ + 1 < segments_.size() &&
           segments_[current_].sequence < playlist.segments.front().sequence) {
      ++current_;
    }
    LOGW("fell behind the live playlist; skipping to segment %lld",
         static_cast<long long>(segments_[current_].sequence));
  }
  // Live: the segments long played are not needed again.
  if (current_ > 2 * kKeptSegments) {
    const size_t drop = current_ - kKeptSegments;
    segments_.erase(segments_.begin(),
                    segments_.begin() + static_cast<ptrdiff_t>(drop));
    current_ -= drop;
  }
  return added;
}

bool HlsDemuxer::OpenSegment(std::string* error) {
  const Segment& segment = segments_[current_];
  if (segment.encrypted) {
    *error = "encrypted HLS (EXT-X-KEY) needs FFmpeg";
    return false;
  }
  if (segment.initialization) {
    *error = "HLS with fragmented MP4 segments (EXT-X-MAP) needs FFmpeg";
    return false;
  }
  std::unique_ptr<DataSource> source = OpenDataSource(segment.url, error);
  if (!source) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    segment_source_ = std::move(source);
  }
  Track(segment_source_.get());
  auto reader = std::make_unique<TsReader>(segment_source_.get());
  const int64_t begin = segment.offset >= 0 ? segment.offset : 0;
  reader->SetRange(begin, segment.length >= 0 ? begin + segment.length : -1);
  // Timestamps unwrap next to where the segment should start.
  auto anchor = offsets_.find(segment.discontinuity);
  reader->Restart(begin, anchor == offsets_.end()
                             ? kNoTimestamp
                             : media_info_.start_time + segment.start -
                                   origin_ - anchor->second);
  if (!reader->Probe(kSegmentProbeBytes, error)) {
    *error = segment.url + ": " + *error;
    return false;
  }

  const bool first = media_info_.streams.empty();
  if (first) {
    for (const TsStream& stream : reader->streams()) {
      media_info_.streams.push_back(stream.info);
      const int index = stream.info.index;
      if (stream.info.type == MediaType::kVideo &&
          media_info_.default_video_stream < 0) {
        media_info_.default_video_stream = index;
      }
      if (stream.info.type == MediaType::kAudio &&
          media_info_.default_audio_stream < 0) {
        media_info_.default_audio_stream = index;
      }
    }
    if (media_info_.streams.empty()) {
      *error = "no H.264, HEVC or audio stream in the HLS segment";
      return false;
    }
  }
  // A segment's n-th video (audio) stream is the media's n-th.
  stream_map_.clear();
  int64_t start = kNoTimestamp;
  for (const TsStream& stream : reader->streams()) {
    int ordinal = 0;
    for (const TsStream& other : reader->streams()) {
      if (other.info.index < stream.info.index &&
          other.info.type == stream.info.type) {
        ++ordinal;
      }
    }
    int index = -1;
    for (const StreamInfo& info : media_info_.streams) {
      if (info.type == stream.info.type && ordinal-- == 0) {
        index = info.index;
        break;
      }
    }
    stream_map_.push_back(index);
    if (index >= 0 && stream.first_pts != kNoTimestamp &&
        (start == kNoTimestamp || stream.first_pts < start)) {
      start = stream.first_pts;
    }
  }
  if (anchor == offsets_.end()) {
    if (start == kNoTimestamp) {
      start = 0;
    }
    if (first) {
      media_info_.start_time = start;
      origin_ = segment.start;
    }
    offset_ = media_info_.start_time + segment.start - origin_ - start;
    offsets_[segment.discontinuity] = offset_;
  } else {
    offset_ = anchor->second;
  }
  segment_reader_ = std::move(reader);
  return true;
}

void HlsDemuxer::CloseSegment() {
  segment_reader_.reset();
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = nullptr;
  segment_source_.reset();
}

DemuxerReadResult HlsDemuxer::WaitForSegments() {
  while (current_ >= segments_.size()) {
    if (ended_) {
      return DemuxerReadResult::kEndOfStream;
    }
    // RFC 8216 6.3.4: a target duration after a reload that changed the
    // playlist, half of one after one that did not.
    const int64_t due =
        last_reload_ +
        (last_reload_changed_ ? target_duration_ : target_duration_ / 2);
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock,
                     std::chrono::microseconds(
                         std::max<int64_t>(due - MonotonicMicros(), 0)),
                     [this] { return aborted_ || interrupted_; });
    }
    if (aborted_ || interrupted_) {
      return DemuxerReadResult::kAborted;
    }
    HlsPlaylist playlist;
    std::string final_url;
    std::string error;
    last_reload_ = MonotonicMicros();
    if (!LoadPlaylist(nullptr, playlist_url_, &playlist, &final_url, &error) ||
        playlist.master) {
      if (Failed()) {
        return DemuxerReadResult::kAborted;
      }
      LOGW("playlist reload failed: %s", error.c_str());
      if (++reload_failures_ >= kMaxReloadFailures) {
        error_ = error.empty() ? "live playlist became a master playlist" : error;
        return DemuxerReadResult::kError;
      }
      last_reload_changed_ = false;
      continue;
    }
    reload_failures_ = 0;
    last_reload_changed_ = Merge(playlist);
    if (playlist.ended) {
      ended_ = true;
    }
  }
  return DemuxerReadResult::kOk;
}

void HlsDemuxer::SetEnabledStreams(const std::vector<int>& stream_indices) {
  std::fill(enabled_.begin(), enabled_.end(), false);
  for (int index : stream_indices) {
    if (index >= 0 && index < static_cast<int>(enabled_.size())) {
      enabled_[index] = true;
    }
  }
}

DemuxerReadResult HlsDemuxer::Read(EncodedPacket* packet) {
  for (;;) {
    if (aborted_ || interrupted_) {
      return DemuxerReadResult::kAborted;
    }
    if (!segment_reader_) {
      if (current_ >= segments_.size()) {
        if (!live_) {
          return DemuxerReadResult::kEndOfStream;
        }
        DemuxerReadResult result = WaitForSegments();
        if (result != DemuxerReadResult::kOk) {
          return result;
        }
        continue;
      }
      std::string error;
      if (!OpenSegment(&error)) {
        const bool failed = Failed();
        CloseSegment();
        if (failed) {
          return DemuxerReadResult::kAborted;
        }
        error_ = error;
        LOGE("%s", error_.c_str());
        return DemuxerReadResult::kError;
      }
    }
    int64_t offset;
    DemuxerReadResult result = segment_reader_->Read(packet, &offset);
    if (result == DemuxerReadResult::kError) {
      if (Failed()) {
        return DemuxerReadResult::kAborted;
      }
      error_ = segments_[current_].url + ": " + segment_reader_->error();
      LOGE("%s", error_.c_str());
      return result;
    }
    if (result != DemuxerReadResult::kOk) {
      CloseSegment();
      ++current_;
      continue;
    }
    const int source_index = packet->stream_index;
    if (source_index < 0 || source_index >= static_cast<int>(stream_map_.size())) {
      continue;
    }
    const int index = stream_map_[source_index];
    if (index < 0 || !enabled_[index]) {
      continue;
    }
    if (media_info_.streams[index].type == MediaType::kVideo) {
      if (need_keyframe_[index] && !packet->keyframe) {
        continue;
      }
      need_keyframe_[index] = false;
    }
    packet->stream_index = index;
    packet->pts += offset_;
    packet->dts += offset_;
    return DemuxerReadResult::kOk;
  }
}

bool HlsDemuxer::Seek(int64_t time) {
  interrupted_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (segment_source_ && !aborted_) {
      segment_source_->ClearInterrupt();
    }
  }
  if (!media_info_.seekable) {
    return false;
  }
  // The segment that holds |time|; decoding starts at its first keyframe.
  const int64_t position = time - media_info_.start_time + origin_;
  size_t index = 0;
  for (size_t i = 0; i < segments_.size(); ++i) {
    if (segments_[i].start <= position) {
      index = i;
    }
  }
  CloseSegment();
  current_ = index;
  std::fill(need_keyframe_.begin(), need_keyframe_.end(), true);
  return true;
}

void HlsDemuxer::Interrupt() {
  interrupted_ = true;
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_) {
    active_->Interrupt();
  }
  wake_.notify_all();
}

void HlsDemuxer::Abort() {
  aborted_ = true;
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_) {
    active_->Interrupt();
  }
  wake_.notify_all();
}

}  // namespace lychee
