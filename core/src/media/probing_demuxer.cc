#define LOG_TAG "probe"

#include "media/probing_demuxer.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "backends/audio/adts_demuxer.h"
#include "backends/audio/flac_demuxer.h"
#include "backends/audio/mp3_demuxer.h"
#include "backends/audio/ogg_demuxer.h"
#include "backends/audio/wav_demuxer.h"
#include "backends/avi/avi_demuxer.h"
#include "backends/flv/flv_demuxer.h"
#include "backends/hls/hls_demuxer.h"
#include "backends/matroska/matroska_demuxer.h"
#include "backends/mp2t/ts_demuxer.h"
#include "backends/mp2t/ts_reader.h"
#include "backends/mp4/mp4_demuxer.h"
#include "backends/mpegps/ps_demuxer.h"
#include "base/log.h"
#include "media/data_source.h"
#if defined(_WIN32)
#include "backends/media_foundation/mf_asf_demuxer.h"
#endif

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

ProbingDemuxer::ProbingDemuxer(Fallback fallback, std::string headers)
    : fallback_(std::move(fallback)), headers_(std::move(headers)) {}
ProbingDemuxer::~ProbingDemuxer() = default;

const char* ProbingDemuxer::name() const {
  return demuxer_ ? demuxer_->name() : "probe";
}

void ProbingDemuxer::Adopt(std::unique_ptr<Demuxer> demuxer) {
  // One that failed to open goes after the unlock: an http source
  // cancels its session.
  std::unique_ptr<Demuxer> replaced;
  std::lock_guard<std::mutex> lock(mutex_);
  replaced = std::move(demuxer_);
  demuxer_ = std::move(demuxer);
  demuxer_->SetVariantPolicy(pinned_, max_bandwidth_);
  if (aborted_) {
    demuxer_->Abort();
  }
}

bool ProbingDemuxer::OpenFallback(std::unique_ptr<DataSource> probed,
                                  const std::string& url,
                                  const std::string& why,
                                  std::string* error) {
  if (aborted_) {
    *error = "aborted";
    return false;
  }
  if (!why.empty()) {
    LOGI("%s; the fallback opens it", why.c_str());
  }
  std::unique_ptr<Demuxer> demuxer = fallback_(std::move(probed));
  Demuxer* opening = demuxer.get();
  Adopt(std::move(demuxer));
  return opening->Open(url, error);
}

bool ProbingDemuxer::Open(const std::string& url, std::string* error) {
  std::unique_ptr<DataSource> source = OpenDataSource(url, error, headers_);
  if (!source) {
    // Another scheme (data:, rtsp://), or http without a client.
    return fallback_ && OpenFallback(nullptr, url, "", error);
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
    // FFmpeg's http reads what App Transport Security refuses.
    source.reset();
    return fallback_ && OpenFallback(nullptr, url, "", error);
  }
  // Each demuxer takes the source with what it read so far (the http
  // source keeps it), so the look costs no second request. A fallback
  // after a failed open reads the URL anew: that is rare (SAMPLE-AES,
  // subtitles other than WebVTT, MPEG-2 video), and the own demuxer may have
  // read anywhere in it.
  if (HlsDemuxer::Detect(head, filled)) {
    auto hls = std::make_unique<HlsDemuxer>();
    HlsDemuxer* opening = hls.get();
    opening->set_refuse_subtitles(fallback_ != nullptr);
    opening->set_opener(CreateSharedOpener(headers_));
    Adopt(std::move(hls));
    const bool opened = opening->Open(std::move(source), url, error);
    if (opened || !fallback_) {
      return opened;
    }
    return OpenFallback(nullptr, url, "hls: " + *error, error);
  }
  if (TsReader::Detect(head, filled)) {
    auto ts = std::make_unique<TsDemuxer>();
    TsDemuxer* opening = ts.get();
    opening->set_refuse_skipped_streams(fallback_ != nullptr);
    Adopt(std::move(ts));
    const bool opened = opening->Open(std::move(source), error);
    if (opened || !fallback_) {
      return opened;
    }
    return OpenFallback(nullptr, url, "mpegts: " + *error, error);
  }
#if defined(_WIN32)
  // The optional FFmpeg ships neither ASF nor WMV/WMA. The system reader
  // and decoders own this path in both builds.
  static constexpr uint8_t kAsfHeader[] = {0x30, 0x26, 0xb2, 0x75, 0x8e, 0x66,
                                           0xcf, 0x11, 0xa6, 0xd9, 0x00, 0xaa,
                                           0x00, 0x62, 0xce, 0x6c};
  if (filled >= sizeof(kAsfHeader) &&
      std::equal(std::begin(kAsfHeader), std::end(kAsfHeader), head)) {
    auto asf = std::make_unique<MFAsfDemuxer>(std::move(source));
    MFAsfDemuxer* opening = asf.get();
    Adopt(std::move(asf));
    return opening->Open(url, error);
  }
#endif
  // The shipped FFmpeg omits AVI and MPEG-PS too. Native readers must
  // stay available when the optional backend is enabled.
  if (AviDemuxer::Detect(head, filled)) {
    auto avi = std::make_unique<AviDemuxer>();
    AviDemuxer* opening = avi.get();
    Adopt(std::move(avi));
    const bool opened = opening->Open(std::move(source), error);
    if (opened || !fallback_) return opened;
    return OpenFallback(nullptr, url, "avi: " + *error, error);
  }
  if (PsDemuxer::Detect(head, filled)) {
    auto ps = std::make_unique<PsDemuxer>();
    PsDemuxer* opening = ps.get();
    Adopt(std::move(ps));
    const bool opened = opening->Open(std::move(source), error);
    if (opened || !fallback_) return opened;
    return OpenFallback(nullptr, url, "ps: " + *error, error);
  }
  if (fallback_) {
    return OpenFallback(std::move(source), url, "", error);
  }
  if (IsEbml(head, filled)) {
    auto matroska = std::make_unique<MatroskaDemuxer>();
    MatroskaDemuxer* opening = matroska.get();
    Adopt(std::move(matroska));
    return opening->Open(std::move(source), error);
  }
  if (Mp3Demuxer::Detect(head, filled)) {
    auto demuxer = std::make_unique<Mp3Demuxer>();
    Mp3Demuxer* opening = demuxer.get();
    Adopt(std::move(demuxer));
    return opening->Open(std::move(source), error);
  }
  if (FlacDemuxer::Detect(head, filled)) {
    auto demuxer = std::make_unique<FlacDemuxer>();
    FlacDemuxer* opening = demuxer.get();
    Adopt(std::move(demuxer));
    return opening->Open(std::move(source), error);
  }
  if (WavDemuxer::Detect(head, filled)) {
    auto demuxer = std::make_unique<WavDemuxer>();
    WavDemuxer* opening = demuxer.get();
    Adopt(std::move(demuxer));
    return opening->Open(std::move(source), error);
  }
  if (AdtsDemuxer::Detect(head, filled)) {
    auto demuxer = std::make_unique<AdtsDemuxer>();
    AdtsDemuxer* opening = demuxer.get();
    Adopt(std::move(demuxer));
    return opening->Open(std::move(source), error);
  }
  if (OggDemuxer::Detect(head, filled)) {
    auto demuxer = std::make_unique<OggDemuxer>();
    OggDemuxer* opening = demuxer.get();
    Adopt(std::move(demuxer));
    return opening->Open(std::move(source), error);
  }
  if (FlvDemuxer::Detect(head, filled)) {
    auto flv = std::make_unique<FlvDemuxer>();
    FlvDemuxer* opening = flv.get();
    Adopt(std::move(flv));
    return opening->Open(std::move(source), error);
  }

  // Everything else is MP4's to open or to name the error for. What it
  // fetches besides (IMSC images) carries the headers too.
  auto mp4 = std::make_unique<Mp4Demuxer>();
  Mp4Demuxer* opening = mp4.get();
  Adopt(std::move(mp4));
  return opening->Open(
      std::move(source), error, {},
      [headers = headers_](const std::string& resource, std::string* why) {
        return OpenDataSource(resource, why, headers);
      });
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

int64_t ProbingDemuxer::download_rate() const {
  return demuxer_ ? demuxer_->download_rate() : -1;
}

std::vector<VariantInfo> ProbingDemuxer::variants() const {
  return demuxer_ ? demuxer_->variants() : std::vector<VariantInfo>();
}

int ProbingDemuxer::current_variant() const {
  return demuxer_ ? demuxer_->current_variant() : -1;
}

void ProbingDemuxer::SetVariantPolicy(int pinned, int64_t max_bandwidth) {
  std::lock_guard<std::mutex> lock(mutex_);
  pinned_ = pinned;
  max_bandwidth_ = max_bandwidth;
  if (demuxer_) {
    demuxer_->SetVariantPolicy(pinned, max_bandwidth);
  }
}

bool ProbingDemuxer::live_window(LiveWindow* window) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return demuxer_ && demuxer_->live_window(window);
}

}  // namespace lychee
