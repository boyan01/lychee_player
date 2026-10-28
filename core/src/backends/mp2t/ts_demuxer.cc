#define LOG_TAG "mpegts"

#include "backends/mp2t/ts_demuxer.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

#include "base/log.h"

namespace lychee {

namespace {

// How much of the input the open may read to find every stream's
// configuration (FFmpeg reads up to 5 MB).
constexpr int64_t kProbeBytes = 4 << 20;
// Bytes looked at for one timestamp while bisecting, and the range at
// which bisection stops and reading takes over.
constexpr int64_t kPtsWindow = 256 << 10;
constexpr int64_t kBisectStop = 512 << 10;
// Tail looked at for the duration, growing up to the maximum.
constexpr int64_t kTailBytes = 1 << 20;
constexpr int64_t kMaxTailBytes = 16 << 20;
// PtsAt() reads its window in chunks, stopping at the first timestamp.
constexpr int64_t kPtsChunk = 32 << 10;
// Timestamps sampled across the input at open for where they start
// over, at least this far apart. A run shorter than the spacing, or one
// that starts ahead of the sample before it, is found when read.
constexpr int64_t kRunSamples = 16;
constexpr int64_t kRunSampleSpacing = 64 << 10;
// A reset found within this many bytes is read across.
constexpr int64_t kRunBisectStop = 64 << 10;
// Packets read after a reset, at least, for the anchor's earliest PTS in
// the new run (video starts with a frame whose PTS is not its earliest).
constexpr int kRunStartPackets = 16;
// Timestamps going back by more than this start a run (as in TsReader);
// a stream's first packet must go back further, since streams are
// interleaved up to about this much apart.
constexpr int64_t kMaxStepBack = kMicrosPerSecond / 2;
constexpr int64_t kMaxInterleave = kMicrosPerSecond;

std::string HexByte(int value) {
  static const char kDigits[] = "0123456789abcdef";
  return {kDigits[(value >> 4) & 0xf], kDigits[value & 0xf]};
}

}  // namespace

TsDemuxer::TsDemuxer() = default;
TsDemuxer::~TsDemuxer() = default;

bool TsDemuxer::Open(const std::string& url, std::string* error) {
  std::unique_ptr<DataSource> source = OpenDataSource(url, error);
  if (!source) {
    return false;
  }
  return Open(std::move(source), error);
}

bool TsDemuxer::Open(std::unique_ptr<DataSource> source, std::string* error) {
  {
    std::lock_guard<std::mutex> lock(source_mutex_);
    source_ = std::move(source);
    if (aborted_) {
      source_->Interrupt();
    }
  }
  timeline_.assign(1, TsRun());
  reader_ = std::make_unique<TsReader>(source_.get());
  reader_->JoinRuns(&timeline_);
  reader_->SetRange(0, -1);
  if (!reader_->Probe(kProbeBytes, error)) {
    if (aborted_) {
      *error = "aborted";
    }
    return false;
  }
  if (refuse_skipped_) {
    const std::vector<int>& skipped = reader_->skipped_stream_types();
    if (!skipped.empty()) {
      *error = "stream type 0x" + HexByte(skipped[0]) + " not supported";
      return false;
    }
  }
  media_info_ = MediaInfo();
  media_info_.format_name = "mpegts";
  int64_t start = kNoTimestamp;
  for (const TsStream& stream : reader_->streams()) {
    media_info_.streams.push_back(stream.info);
    if (stream.first_pts != kNoTimestamp &&
        (start == kNoTimestamp || stream.first_pts < start)) {
      start = stream.first_pts;
    }
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
    *error = "no H.264, HEVC or audio stream in the MPEG-TS program";
    return false;
  }
  media_info_.start_time = start == kNoTimestamp ? 0 : start;
  media_info_.seekable = source_->seekable() && source_->size() > 0;
  if (media_info_.seekable) {
    int reference = media_info_.default_video_stream;
    if (reference < 0) {
      reference = 0;
    }
    pts_cache_.clear();
    if (FindRuns(reader_->streams()[reference].pid)) {
      EstimateDuration();
    }
    pts_cache_.clear();
  }
  enabled_.assign(media_info_.streams.size(), true);
  need_keyframe_.assign(media_info_.streams.size(), true);
  return true;
}

void TsDemuxer::EstimateDuration() {
  const int64_t size = source_->size();
  for (int64_t tail = kTailBytes; tail <= kMaxTailBytes; tail *= 4) {
    const int64_t begin = std::max(reader_->data_start(), size - tail);
    TsReader reader(source_.get());
    reader.AdoptProgram(*reader_);
    reader.JoinRuns(&timeline_);
    reader.SetRange(begin, size);
    int64_t end = kNoTimestamp;
    EncodedPacket packet;
    int64_t offset;
    DemuxerReadResult result;
    while ((result = reader.Read(&packet, &offset)) == DemuxerReadResult::kOk) {
      const int64_t packet_end =
          packet.pts + std::max<int64_t>(packet.duration, 0);
      if (end == kNoTimestamp || packet_end > end) {
        end = packet_end;
      }
    }
    if (end != kNoTimestamp) {
      // On the timeline of the runs, unwrapped from each run's start.
      const int64_t duration = end - media_info_.start_time;
      if (duration > 0) {
        media_info_.duration = duration;
      }
      return;
    }
    // A tail without sync bytes (damage) is no reason to stop looking;
    // a failed read is.
    if (begin == reader_->data_start() ||
        (result == DemuxerReadResult::kError && Failed())) {
      return;
    }
  }
}

bool TsDemuxer::FindRuns(int pid) {
  const int64_t begin = reader_->data_start();
  const int64_t size = source_->size();
  const int64_t span = size - begin;
  const int64_t count =
      std::clamp<int64_t>(span / kRunSampleSpacing, 1, kRunSamples);
  std::vector<std::pair<int64_t, int64_t>> samples;
  int64_t near = media_info_.start_time;
  for (int64_t i = 0; i <= count; ++i) {
    const int64_t offset = i < count
                               ? begin + span * i / count
                               : std::max(begin, size - kRunSampleSpacing);
    int64_t pts;
    bool read_error;
    if (PtsAt(offset, pid, near, &pts, &read_error)) {
      samples.emplace_back(offset, pts);
      near = pts;
    } else if (read_error) {
      return false;
    }
  }
  for (size_t i = 0; i + 1 < samples.size(); ++i) {
    if (samples[i + 1].second < samples[i].second - kMaxStepBack &&
        !LocateRun(pid, samples[i], samples[i + 1])) {
      return false;
    }
  }
  return true;
}

bool TsDemuxer::LocateRun(int pid,
                          std::pair<int64_t, int64_t> before,
                          std::pair<int64_t, int64_t> after) {
  // Timestamps go up within a run, and a later run's stay below the
  // earlier sample until |after|: bisect on which side a PTS falls.
  const int64_t size = reader_->packet_size();
  int64_t low = before.first;
  int64_t high = after.first;
  int64_t last = before.second;
  while (high - low > kRunBisectStop) {
    int64_t middle = low + (high - low) / 2;
    middle = low + (middle - low) / size * size;
    int64_t pts;
    bool read_error;
    if (!PtsAt(middle, pid, last, &pts, &read_error)) {
      if (read_error) {
        return false;
      }
      high = middle;
    } else if (pts >= last - kMaxStepBack) {
      low = middle;
      last = pts;
    } else {
      high = middle;
    }
  }
  // Read across the reset, from far enough before it for the frame that
  // shows last, which with B-frames is not the last one decoded. Packets
  // come out as their PES ends, so one stream's first packets of the new
  // run may come before another's last of the old one: each stream
  // moves on when its own DTS goes back.
  TsReader reader(source_.get());
  reader.AdoptProgram(*reader_);
  // Runs are found in input order: the one known at |low| is its own.
  const int64_t run_offset = timeline_[TsReader::RunAt(timeline_, low)].offset;
  const int64_t from = std::max(run_offset, low - kRunBisectStop);
  reader.SetRange(from, std::min(source_->size(), high + kPtsWindow));
  // A stream's first packet is in the old run if near where reading
  // starts.
  int64_t first = last;
  bool read_error;
  if (from < low && !PtsAt(from, pid, last, &first, &read_error)) {
    if (read_error) {
      return false;
    }
    first = last;
  }
  // The anchor stream's frames decide the join (TsRun).
  const int anchor = media_info_.default_video_stream >= 0
                         ? media_info_.default_video_stream
                         : media_info_.default_audio_stream;
  // Per stream: its last DTS in the old run, or kNoTimestamp once moved.
  std::map<int, int64_t> dts;
  int64_t end = kNoTimestamp;
  int64_t reset = -1;
  int64_t start = kNoTimestamp;
  int read_after = 0;
  EncodedPacket packet;
  int64_t offset;
  DemuxerReadResult result;
  while ((result = reader.Read(&packet, &offset)) == DemuxerReadResult::kOk) {
    const int index = packet.stream_index;
    auto it = dts.find(index);
    const bool moved = it != dts.end()
                           ? it->second == kNoTimestamp ||
                                 packet.dts < it->second - kMaxStepBack
                           : packet.dts < first - kMaxInterleave;
    if (!moved) {
      dts[index] = packet.dts;
      if (index == anchor) {
        end = std::max(end == kNoTimestamp ? packet.pts : end,
                       packet.pts + std::max<int64_t>(packet.duration, 0));
      }
      continue;
    }
    dts[index] = kNoTimestamp;
    if (reset < 0 || offset < reset) {
      reset = offset;
    }
    if (index == anchor && (start == kNoTimestamp || packet.pts < start)) {
      start = packet.pts;
    }
    if (++read_after >= kRunStartPackets && start != kNoTimestamp) {
      break;
    }
  }
  if (result == DemuxerReadResult::kError && Failed()) {
    return false;
  }
  if (reset < 0 || end == kNoTimestamp || start == kNoTimestamp) {
    return true;  // Left to reading.
  }
  AddRun(reset, start, end);
  if (after.second < start - kMaxStepBack) {
    return LocateRun(pid, {reset, start}, after);
  }
  return true;
}

void TsDemuxer::AddRun(int64_t offset, int64_t start, int64_t end) {
  for (const TsRun& run : timeline_) {
    if (run.start != kNoTimestamp &&
        std::abs(run.offset - offset) < kPtsWindow &&
        std::abs(TsReader::TicksToMicros(
            TsReader::Nearest(TsReader::MicrosToTicks(start),
                              TsReader::MicrosToTicks(run.start)) -
            TsReader::MicrosToTicks(run.start))) <= kMaxStepBack * 2) {
      return;  // Reading found it already.
    }
  }
  const size_t before = TsReader::RunAt(timeline_, offset);
  const TsRun& previous = timeline_[before];
  // |end| unwrapped as the run before it unwraps (whole 2^33 wraps, so
  // through ticks without rounding |end|).
  const int64_t end_ticks = TsReader::MicrosToTicks(end);
  const int64_t previous_end =
      end +
      TsReader::TicksToMicros(
          TsReader::Nearest(end_ticks,
                            TsReader::MicrosToTicks(previous.start)) -
          end_ticks) +
      previous.shift;
  TsRun run;
  run.offset = offset;
  run.start = start;
  run.shift = previous_end - start;
  LOGI("timestamps start over at byte %lld: %lld us on from %lld us",
       static_cast<long long>(offset), static_cast<long long>(run.shift),
       static_cast<long long>(start));
  timeline_.insert(timeline_.begin() + static_cast<ptrdiff_t>(before) + 1, run);
}

void TsDemuxer::SetEnabledStreams(const std::vector<int>& stream_indices) {
  std::fill(enabled_.begin(), enabled_.end(), false);
  for (int index : stream_indices) {
    if (index >= 0 && index < static_cast<int>(enabled_.size())) {
      enabled_[index] = true;
    }
  }
}

bool TsDemuxer::Failed() {
  return aborted_ || interrupted_ || source_->interrupted();
}

DemuxerReadResult TsDemuxer::Read(EncodedPacket* packet) {
  for (;;) {
    if (aborted_ || interrupted_) {
      return DemuxerReadResult::kAborted;
    }
    int64_t offset = 0;
    DemuxerReadResult result = reader_->Read(packet, &offset);
    if (result == DemuxerReadResult::kError) {
      if (Failed()) {
        return DemuxerReadResult::kAborted;
      }
      error_ = reader_->error();
      LOGE("read failed: %s", error_.c_str());
      return result;
    }
    if (result != DemuxerReadResult::kOk) {
      return result;
    }
    const int index = packet->stream_index;
    if (index < 0 || index >= static_cast<int>(enabled_.size()) ||
        !enabled_[index]) {
      continue;
    }
    const MediaType type = media_info_.streams[index].type;
    if (type == MediaType::kVideo) {
      if (offset < video_from_offset_ ||
          (need_keyframe_[index] && !packet->keyframe)) {
        continue;
      }
      need_keyframe_[index] = false;
    } else if (audio_from_ != kNoTimestamp &&
               packet->pts + packet->duration <= audio_from_) {
      continue;
    }
    return DemuxerReadResult::kOk;
  }
}

bool TsDemuxer::PtsAt(int64_t offset,
                      int pid,
                      int64_t near,
                      int64_t* pts,
                      bool* read_error) {
  *read_error = false;
  const int64_t size = reader_->packet_size();
  const int64_t start = reader_->data_start();
  offset = start + std::max<int64_t>(offset - start, 0) / size * size;
  // Bisections for a seek's successive back-offs visit the same offsets.
  auto cached = pts_cache_.find(offset);
  if (cached != pts_cache_.end()) {
    if (cached->second < 0) {
      return false;
    }
    *pts = TsReader::TicksToMicros(
        TsReader::Nearest(cached->second, TsReader::MicrosToTicks(near)));
    return true;
  }
  // In chunks: the first timestamp is usually within one.
  const size_t limit = static_cast<size_t>(kPtsWindow / size * size);
  const size_t chunk = static_cast<size_t>(kPtsChunk / size * size);
  std::vector<uint8_t> window;
  const size_t prefix = static_cast<size_t>(size) - kTsPacketSize;
  size_t p = 0;
  bool eof = false;
  while (!eof && window.size() < limit) {
    const size_t filled = window.size();
    window.resize(std::min(limit, filled + chunk));
    size_t got = filled;
    while (got < window.size()) {
      int64_t n = source_->Read(offset + static_cast<int64_t>(got),
                                window.data() + got, window.size() - got);
      if (n < 0) {
        *read_error = true;
        return false;
      }
      if (n == 0) {
        eof = true;
        break;
      }
      got += static_cast<size_t>(n);
    }
    window.resize(got);
    while (p + static_cast<size_t>(size) <= window.size()) {
      const uint8_t* packet = window.data() + p + prefix;
      if (packet[0] != 0x47) {
        ++p;  // Out of sync: look for the next sync byte.
        continue;
      }
      p += static_cast<size_t>(size);
      const bool unit_start = packet[1] & 0x40;
      const int packet_pid = ((packet[1] & 0x1f) << 8) | packet[2];
      const int adaptation = (packet[3] >> 4) & 3;
      if (!unit_start || packet_pid != pid || !(adaptation & 1)) {
        continue;
      }
      size_t at = 4;
      if (adaptation & 2) {
        at += 1 + packet[4];
      }
      // PES start code, stream id, length, flags, header length, PTS.
      if (at + 14 > kTsPacketSize || packet[at] != 0 || packet[at + 1] != 0 ||
          packet[at + 2] != 1 || !(packet[at + 7] & 0x80)) {
        continue;
      }
      const uint8_t* field = packet + at + 9;
      const int64_t raw = (static_cast<int64_t>((field[0] >> 1) & 7) << 30) |
                          (static_cast<int64_t>(field[1]) << 22) |
                          (static_cast<int64_t>(field[2] >> 1) << 15) |
                          (static_cast<int64_t>(field[3]) << 7) |
                          (field[4] >> 1);
      pts_cache_[offset] = raw;
      *pts = TsReader::TicksToMicros(
          TsReader::Nearest(raw, TsReader::MicrosToTicks(near)));
      return true;
    }
  }
  pts_cache_[offset] = -1;
  return false;
}

int64_t TsDemuxer::Bisect(int pid, int64_t time) {
  // Timestamps go up only within a run: bisect in the one playing |time|,
  // on its timestamps as read.
  size_t index = 0;
  for (size_t i = 1; i < timeline_.size(); ++i) {
    const TsRun& run = timeline_[i];
    if (run.start != kNoTimestamp && run.start + run.shift <= time) {
      index = i;
    }
  }
  const TsRun& run = timeline_[index];
  time -= run.shift;
  const int64_t size = reader_->packet_size();
  int64_t low = std::max(reader_->data_start(), run.offset);
  int64_t high = index + 1 < timeline_.size() ? timeline_[index + 1].offset
                                              : source_->size();
  while (high - low > kBisectStop) {
    int64_t middle = low + (high - low) / 2;
    middle = low + (middle - low) / size * size;
    int64_t pts;
    bool read_error;
    if (!PtsAt(middle, pid, time, &pts, &read_error)) {
      if (read_error) {
        return -1;
      }
      high = middle;
    } else if (pts <= time) {
      low = middle;
    } else {
      high = middle;
    }
  }
  return low;
}

bool TsDemuxer::Seek(int64_t time) {
  interrupted_ = false;
  if (!aborted_) {
    source_->ClearInterrupt();
  }
  if (!media_info_.seekable) {
    return false;
  }
  // Video lands on the keyframe at or before |time|; audio on the frame
  // that plays at that keyframe's time.
  int reference = -1;
  for (const StreamInfo& stream : media_info_.streams) {
    if (enabled_[stream.index] && stream.type == MediaType::kVideo) {
      reference = stream.index;
      break;
    }
  }
  for (const StreamInfo& stream : media_info_.streams) {
    if (reference < 0 && enabled_[stream.index]) {
      reference = stream.index;
    }
  }
  if (reference < 0) {
    reference = 0;
  }
  const bool video = media_info_.streams[reference].type == MediaType::kVideo;
  const int pid = reader_->streams()[reference].pid;
  const int64_t data_start = reader_->data_start();
  pts_cache_.clear();

  int64_t found_offset = -1;
  int64_t found_pts = kNoTimestamp;
  // Audio frames read before the reference's stop: (pts, PES offset).
  std::vector<std::pair<int64_t, int64_t>> audio;
  // Back off further each time the window held no keyframe. Each window
  // reads only up to where the previous one began (plus the audio around
  // a keyframe found there): long GOPs made rereading the whole range a
  // seek's main cost.
  int64_t scanned_from = -1;
  for (int64_t backoff = kMicrosPerSecond;; backoff *= 2) {
    const bool from_start = backoff > 64 * kMicrosPerSecond;
    int64_t start = from_start ? data_start : Bisect(pid, time - backoff);
    if (start < 0) {
      if (!Failed()) {
        error_ = source_->error();
        LOGE("seek failed: %s", error_.c_str());
      }
      return false;
    }
    if (scanned_from >= 0 && start >= scanned_from) {
      if (scanned_from == data_start) {
        break;
      }
      continue;  // Bisection landed in the window already read.
    }
    reader_->Restart(start, time);
    audio.clear();
    EncodedPacket packet;
    int64_t offset;
    for (;;) {
      DemuxerReadResult result = reader_->Read(&packet, &offset);
      if (result == DemuxerReadResult::kError) {
        if (!Failed()) {
          error_ = reader_->error();
          LOGE("seek failed: %s", error_.c_str());
        }
        return false;
      }
      if (result != DemuxerReadResult::kOk) {
        break;
      }
      const int index = packet.stream_index;
      if (index == reference) {
        if (video ? packet.dts > time : packet.pts > time) {
          break;
        }
        if (scanned_from >= 0 && offset >= scanned_from &&
            (found_offset < 0 || packet.pts > found_pts + kMicrosPerSecond)) {
          break;
        }
        if ((!video || packet.keyframe) && packet.pts <= time) {
          found_offset = offset;
          found_pts = packet.pts;
          // Older audio cannot be the frame at a later keyframe.
          std::erase_if(audio, [&](const auto& entry) {
            return entry.first < found_pts - kMicrosPerSecond;
          });
        }
      } else if (index < static_cast<int>(enabled_.size()) && enabled_[index] &&
                 media_info_.streams[index].type == MediaType::kAudio) {
        audio.emplace_back(packet.pts, offset);
      }
    }
    if (found_offset >= 0 || start == data_start) {
      break;
    }
    scanned_from = start;
  }

  std::fill(need_keyframe_.begin(), need_keyframe_.end(), true);
  if (found_offset < 0) {
    // Before the first keyframe: play from the start.
    reader_->Restart(data_start, media_info_.start_time);
    video_from_offset_ = -1;
    audio_from_ = kNoTimestamp;
    return true;
  }
  int64_t start = found_offset;
  int64_t audio_pts = kNoTimestamp;
  for (const auto& [pts, offset] : audio) {
    if (pts <= found_pts && (audio_pts == kNoTimestamp || pts >= audio_pts)) {
      audio_pts = pts;
      start = std::min(found_offset, offset);
    }
  }
  reader_->Restart(start, found_pts);
  video_from_offset_ = video ? found_offset : -1;
  audio_from_ = found_pts;
  return true;
}

void TsDemuxer::Interrupt() {
  interrupted_ = true;
  std::lock_guard<std::mutex> lock(source_mutex_);
  if (source_) {
    source_->Interrupt();
  }
}

void TsDemuxer::Abort() {
  aborted_ = true;
  std::lock_guard<std::mutex> lock(source_mutex_);
  if (source_) {
    source_->Interrupt();
  }
}

}  // namespace lychee
