#define LOG_TAG "live"

#include "pipeline/live_demuxer.h"

#include "base/log.h"
#include "base/time.h"

namespace lychee {

namespace {

// Queued data beyond this means the decoders stopped taking packets; start
// over from the next keyframe rather than grow without bound.
constexpr size_t kMaxQueuedBytes = size_t{16} * 1024 * 1024;

constexpr uint8_t kStartCode[] = {0, 0, 0, 1};

bool IsParameterSet(int type, bool hevc) {
  // HEVC VPS, SPS, PPS; H.264 SPS, PPS.
  return hevc ? type >= 32 && type <= 34 : type == 7 || type == 8;
}

bool IsKeyframe(int type, bool hevc) {
  // HEVC IRAP pictures; H.264 IDR.
  return hevc ? type >= 16 && type <= 23 : type == 5;
}

}  // namespace

LiveDemuxer::LiveDemuxer(int64_t latency,
                         const std::atomic<int64_t>* audio_output_lead)
    : audio_output_lead_(audio_output_lead), timeline_(latency) {
  // The media timeline is the NowMicros() timebase; the position counts
  // from lychee_player_open_live(), on the caller's thread.
  media_info_.start_time = NowMicros();
  media_info_.format_name = "live";
  media_info_.streams.resize(2);
  media_info_.streams[kVideoStream].index = kVideoStream;
  media_info_.streams[kAudioStream].index = kAudioStream;
}

LiveDemuxer::~LiveDemuxer() = default;

bool LiveDemuxer::Open(const std::string&, std::string*) {
  return true;
}

DemuxerReadResult LiveDemuxer::Read(EncodedPacket* packet) {
  std::unique_lock<std::mutex> lock(mutex_);
  in_flight_ = false;
  for (;;) {
    if (aborted_ || interrupted_) {
      return DemuxerReadResult::kAborted;
    }
    Stream* next = nullptr;
    for (Stream& stream : streams_) {
      if (ReadableLocked(stream) &&
          (!next || stream.packets.front()->pts < next->packets.front()->pts)) {
        next = &stream;
      }
    }
    if (next) {
      *packet = *next->packets.front();
      next->packets.pop_front();
      bytes_ -= packet->size();
      in_flight_ = true;
      return DemuxerReadResult::kOk;
    }
    condition_.wait(lock);
  }
}

bool LiveDemuxer::Seek(int64_t) {
  std::lock_guard<std::mutex> lock(mutex_);
  interrupted_ = false;
  return true;
}

void LiveDemuxer::Interrupt() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    interrupted_ = true;
  }
  condition_.notify_all();
}

void LiveDemuxer::Abort() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
  }
  condition_.notify_all();
}

void LiveDemuxer::SetTrack(int index, const StreamInfo& info) {
  std::lock_guard<std::mutex> lock(mutex_);
  Stream& stream = streams_[index];
  DropLocked(&stream);
  stream.has_codec = info.type != MediaType::kUnknown;
  stream.hevc = info.video.codec == VideoCodec::kHEVC;
  stream.held = true;
  stream.hold++;
  stream.disabled = false;
  stream.wait_for_keyframe = true;
  if (index == kVideoStream) {
    parameter_sets_.clear();
  }
}

void LiveDemuxer::Flush(int index) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i : {kVideoStream, kAudioStream}) {
    if (index >= 0 && i != index) {
      continue;
    }
    Stream& stream = streams_[i];
    DropLocked(&stream);
    stream.held = true;
    stream.hold++;
    stream.wait_for_keyframe = true;
  }
  if (index < 0) {
    timeline_.Reset();
  }
}

bool LiveDemuxer::Push(int index,
                       int64_t pts,
                       const uint8_t* data,
                       size_t size) {
  if (index != kVideoStream && index != kAudioStream) {
    return false;
  }
  const int64_t now = NowMicros();
  const int64_t lead =
      audio_output_lead_ ? audio_output_lead_->load(std::memory_order_relaxed)
                         : 0;
  std::lock_guard<std::mutex> lock(mutex_);
  Stream& stream = streams_[index];
  if (!stream.has_codec) {
    return false;
  }
  // Every arrival tells the timeline about transit, kept or not.
  const int64_t deadline = index == kVideoStream
                               ? timeline_.Video(pts, now)
                               : timeline_.Audio(pts, lead, now);
  if (stream.disabled || aborted_) {
    return true;
  }
  if (bytes_ + size > kMaxQueuedBytes) {
    LOGW("%zu bytes queued; dropping them up to the next keyframe", bytes_);
    for (Stream& s : streams_) {
      DropLocked(&s);
      s.wait_for_keyframe = true;
    }
  }
  auto packet = std::make_shared<EncodedPacket>();
  packet->stream_index = index;
  packet->pts = deadline;
  packet->buffer = PacketBuffer::Copy(data, size);
  if (index == kVideoStream) {
    if (!PrepareVideoLocked(&stream, packet.get())) {
      return true;
    }
  } else {
    packet->keyframe = true;
  }
  bytes_ += packet->size();
  stream.packets.push_back(std::move(packet));
  condition_.notify_all();
  return true;
}

bool LiveDemuxer::PrepareVideoLocked(Stream* stream, EncodedPacket* packet) {
  const uint8_t* data = packet->buffer->data();
  const size_t size = packet->size();
  std::vector<uint8_t> sets;
  bool keyframe = false;
  // NAL units, each from after its start code to the next start code.
  size_t begin = size;
  auto finish = [&](size_t end) {
    if (begin >= end) {
      return;
    }
    const int type = stream->hevc ? (data[begin] >> 1) & 0x3f : data[begin] & 0x1f;
    if (IsParameterSet(type, stream->hevc)) {
      sets.insert(sets.end(), std::begin(kStartCode), std::end(kStartCode));
      sets.insert(sets.end(), data + begin, data + end);
    }
    keyframe = keyframe || IsKeyframe(type, stream->hevc);
  };
  for (size_t i = 0; i + 3 <= size;) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      // A four-byte start code's leading zero ends the previous unit.
      finish(i > 0 && data[i - 1] == 0 ? i - 1 : i);
      begin = i + 3;
      i += 3;
    } else {
      ++i;
    }
  }
  finish(size);

  if (!sets.empty()) {
    parameter_sets_ = std::move(sets);
  } else if (keyframe && !parameter_sets_.empty()) {
    std::vector<uint8_t> with_sets = parameter_sets_;
    with_sets.insert(with_sets.end(), data, data + size);
    packet->buffer = PacketBuffer::Copy(with_sets.data(), with_sets.size());
  }
  if (stream->wait_for_keyframe) {
    if (!keyframe) {
      return false;
    }
    stream->wait_for_keyframe = false;
  }
  packet->keyframe = keyframe;
  return true;
}

uint64_t LiveDemuxer::hold(int index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return streams_[index].hold;
}

void LiveDemuxer::Release(int index, bool enabled, uint64_t hold) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Stream& stream = streams_[index];
    if (hold != stream.hold) {
      // A newer change holds it; its Release() decides.
      return;
    }
    stream.held = false;
    stream.disabled = !enabled;
    if (!enabled) {
      DropLocked(&stream);
    }
  }
  condition_.notify_all();
}

void LiveDemuxer::RestartVideo() {
  std::lock_guard<std::mutex> lock(mutex_);
  Stream& stream = streams_[kVideoStream];
  while (!stream.packets.empty() && !stream.packets.front()->keyframe) {
    bytes_ -= stream.packets.front()->size();
    stream.packets.pop_front();
  }
  if (stream.packets.empty()) {
    stream.wait_for_keyframe = true;
  }
}

void LiveDemuxer::PacketDelivered() {
  std::lock_guard<std::mutex> lock(mutex_);
  in_flight_ = false;
}

bool LiveDemuxer::Idle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !in_flight_ && !ReadableLocked(streams_[kVideoStream]) &&
         !ReadableLocked(streams_[kAudioStream]);
}

void LiveDemuxer::DropLocked(Stream* stream) {
  for (const auto& packet : stream->packets) {
    bytes_ -= packet->size();
  }
  stream->packets.clear();
}

bool LiveDemuxer::ReadableLocked(const Stream& stream) const {
  return !stream.held && !stream.disabled && !stream.packets.empty();
}

}  // namespace lychee
