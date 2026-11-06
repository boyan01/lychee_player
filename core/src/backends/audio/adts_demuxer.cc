#include "backends/audio/adts_demuxer.h"
#include <limits>
namespace lychee {
bool AdtsDemuxer::Header(int64_t at, AdtsHeader* h) {
  uint8_t bytes[9];
  const int64_t count = Bytes(at, bytes, sizeof(bytes));
  return count >= 7 && ParseAdtsHeader(bytes, static_cast<size_t>(count), h) &&
         (rate_ == 0 || h->sample_rate == rate_);
}
bool AdtsDemuxer::Parse() {
  AdtsHeader h;
  if (!Header(0, &h)) return false;
  rate_ = h.sample_rate;
  stream_.audio = {AudioCodec::kAAC, "aac", rate_, h.channels};
  points_.push_back({0, 0});
  // Raw ADTS has no duration/index. Build a sparse index using headers
  // only, retaining one point per second rather than every AAC frame.
  if (file_size_ >= 0 && source_->seekable()) {
    if (!Index(std::numeric_limits<int64_t>::max())) return false;
    media_info_.duration = indexed_samples_ * kMicrosPerSecond / rate_;
  }
  return true;
}
bool AdtsDemuxer::Index(int64_t until) {
  while (indexed_samples_ < until &&
         (file_size_ < 0 || indexed_end_ < file_size_)) {
    AdtsHeader h;
    if (!Header(indexed_end_, &h)) return false;
    if (file_size_ >= 0 &&
        h.frame_length > static_cast<uint64_t>(file_size_ - indexed_end_))
      return false;
    if (indexed_samples_ - points_.back().samples >= rate_)
      points_.push_back({indexed_end_, indexed_samples_});
    indexed_end_ += static_cast<int64_t>(h.frame_length);
    indexed_samples_ += h.samples;
    if (points_.size() > 1000000 || Cancelled()) return false;
  }
  return true;
}
DemuxerReadResult AdtsDemuxer::Read(EncodedPacket* packet) {
  if (Cancelled()) return DemuxerReadResult::kAborted;
  if (!enabled_ || (file_size_ >= 0 && position_ == file_size_))
    return DemuxerReadResult::kEndOfStream;
  AdtsHeader h;
  if (!Header(position_, &h)) {
    if (file_size_ == position_ && error_.empty() && !Cancelled())
      return DemuxerReadResult::kEndOfStream;
    return Failure();
  }
  const int64_t pts = samples_ * kMicrosPerSecond / rate_;
  const int64_t end = (samples_ + h.samples) * kMicrosPerSecond / rate_;
  if (!Packet(position_, h.frame_length, pts, end - pts, packet))
    return Failure();
  position_ += static_cast<int64_t>(h.frame_length);
  samples_ += h.samples;
  return DemuxerReadResult::kOk;
}
bool AdtsDemuxer::Seek(int64_t time) {
  if (!BeginSeek()) return false;
  const int64_t target =
      std::clamp<int64_t>(time, 0, std::max<int64_t>(0, media_info_.duration)) *
      rate_ / kMicrosPerSecond;
  auto it = std::upper_bound(points_.begin(), points_.end(), target,
                             [](int64_t samples, const Point& point) {
                               return samples < point.samples;
                             });
  const auto& point = it == points_.begin() ? points_.front() : *std::prev(it);
  position_ = point.offset;
  samples_ = point.samples;
  for (;;) {
    AdtsHeader h;
    if (!Header(position_, &h)) return samples_ >= target;
    if (samples_ + h.samples > target) return true;
    position_ += static_cast<int64_t>(h.frame_length);
    samples_ += h.samples;
  }
}
}  // namespace lychee
