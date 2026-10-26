#include "media/probing_demuxer.h"

#include <utility>

#include "backends/hls/hls_demuxer.h"
#include "backends/matroska/matroska_demuxer.h"
#include "backends/mp2t/ts_demuxer.h"
#include "backends/mp2t/ts_reader.h"
#include "backends/mp4/mp4_demuxer.h"
#include "media/data_source.h"

namespace lychee {

namespace {

// Enough for three TS packets (M2TS: 192 bytes) and a playlist's header.
constexpr size_t kProbeBytes = 1024;

// An EBML header element starts every Matroska and WebM file.
bool IsEbml(const uint8_t* data, size_t size) {
  return size >= 4 && data[0] == 0x1a && data[1] == 0x45 && data[2] == 0xdf &&
         data[3] == 0xa3;
}

}  // namespace

ProbingDemuxer::ProbingDemuxer() = default;
ProbingDemuxer::~ProbingDemuxer() = default;

const char* ProbingDemuxer::name() const {
  return demuxer_ ? demuxer_->name() : "probe";
}

void ProbingDemuxer::Adopt(std::unique_ptr<Demuxer> demuxer) {
  std::lock_guard<std::mutex> lock(mutex_);
  demuxer_ = std::move(demuxer);
  if (aborted_) {
    demuxer_->Abort();
  }
}

bool ProbingDemuxer::Open(const std::string& url, std::string* error) {
  std::unique_ptr<DataSource> source = OpenDataSource(url, error);
  if (!source) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    probing_ = source.get();
    if (aborted_) {
      source->Interrupt();
    }
  }
  uint8_t head[kProbeBytes];
  size_t filled = 0;
  bool failed = false;
  while (filled < sizeof(head)) {
    int64_t n = source->Read(static_cast<int64_t>(filled), head + filled,
                             sizeof(head) - filled);
    if (n < 0) {
      *error = aborted_ ? "aborted" : source->error();
      failed = true;
      break;
    }
    if (n == 0) {
      break;
    }
    filled += static_cast<size_t>(n);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    probing_ = nullptr;
  }
  if (failed) {
    return false;
  }
  // Each demuxer takes the source with what it read so far (the http
  // source keeps it), so the look costs no second request.
  if (HlsDemuxer::Detect(head, filled)) {
    auto hls = std::make_unique<HlsDemuxer>();
    HlsDemuxer* opening = hls.get();
    Adopt(std::move(hls));
    return opening->Open(std::move(source), url, error);
  }
  if (TsReader::Detect(head, filled)) {
    auto ts = std::make_unique<TsDemuxer>();
    TsDemuxer* opening = ts.get();
    Adopt(std::move(ts));
    return opening->Open(std::move(source), error);
  }
  if (IsEbml(head, filled)) {
    auto matroska = std::make_unique<MatroskaDemuxer>();
    MatroskaDemuxer* opening = matroska.get();
    Adopt(std::move(matroska));
    return opening->Open(std::move(source), error);
  }
  // Everything else is MP4's to open or to name the error for.
  auto mp4 = std::make_unique<Mp4Demuxer>();
  Mp4Demuxer* opening = mp4.get();
  Adopt(std::move(mp4));
  return opening->Open(std::move(source), error);
}

const MediaInfo& ProbingDemuxer::media_info() const {
  return demuxer_ ? demuxer_->media_info() : empty_;
}

void ProbingDemuxer::SetEnabledStreams(const std::vector<int>& stream_indices) {
  if (demuxer_) {
    demuxer_->SetEnabledStreams(stream_indices);
  }
}

DemuxerReadResult ProbingDemuxer::Read(EncodedPacket* packet) {
  return demuxer_ ? demuxer_->Read(packet) : DemuxerReadResult::kError;
}

bool ProbingDemuxer::Seek(int64_t time) {
  return demuxer_ && demuxer_->Seek(time);
}

void ProbingDemuxer::Interrupt() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (demuxer_) {
    demuxer_->Interrupt();
  }
}

void ProbingDemuxer::Abort() {
  aborted_ = true;
  std::lock_guard<std::mutex> lock(mutex_);
  if (probing_) {
    probing_->Interrupt();
  }
  if (demuxer_) {
    demuxer_->Abort();
  }
}

std::string ProbingDemuxer::error() const {
  return demuxer_ ? demuxer_->error() : "";
}

}  // namespace lychee
