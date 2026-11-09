#define LOG_TAG "dash"

#include "backends/dash/dash_demuxer.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "base/log.h"

namespace lychee {

namespace {

// As large as an HLS playlist may be.
constexpr int64_t kMaxManifestBytes = 16 << 20;
// A 'sidx' of 65535 references is 786 kB.
constexpr int64_t kMaxIndexBytes = 1 << 20;

}  // namespace

DashDemuxer::DashDemuxer() {
  set_format_name("dash");
  set_playlist_source(
      [this](const std::string& url, HlsPlaylist* playlist,
             std::string* error) { return Playlist(url, playlist, error); });
}

DashDemuxer::~DashDemuxer() = default;

bool DashDemuxer::Detect(const uint8_t* data, size_t size) {
  std::string_view head(reinterpret_cast<const char*>(data), size);
  if (head.substr(0, 3) == "\xef\xbb\xbf") {
    head.remove_prefix(3);
  }
  const size_t first = head.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos || head[first] != '<') {
    return false;
  }
  // After the XML declaration and comments, if any; a prefixed root too.
  return head.find("<MPD") != std::string_view::npos ||
         head.find(":MPD ") != std::string_view::npos;
}

bool DashDemuxer::Fetch(const std::string& url,
                        int64_t offset,
                        int64_t length,
                        std::string* data,
                        std::string* error) {
  std::unique_ptr<DataSource> source = opener()(url, error);
  if (!source) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(fetch_mutex_);
    if (aborted_) {
      *error = "aborted";
      return false;
    }
    fetching_ = source.get();
  }
  const int64_t limit = length >= 0 ? length : kMaxManifestBytes + 1;
  const int64_t begin = std::max<int64_t>(offset, 0);
  uint8_t chunk[16384];
  bool ok = true;
  while (static_cast<int64_t>(data->size()) < limit) {
    const size_t want = static_cast<size_t>(std::min<int64_t>(
        sizeof(chunk), limit - static_cast<int64_t>(data->size())));
    const int64_t n =
        source->Read(begin + static_cast<int64_t>(data->size()), chunk, want);
    if (n < 0) {
      *error = url + ": " + source->error();
      ok = false;
      break;
    }
    if (n == 0) {
      break;
    }
    data->append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n));
  }
  if (ok && length < 0 &&
      static_cast<int64_t>(data->size()) > kMaxManifestBytes) {
    *error = url + ": manifest too large";
    ok = false;
  }
  if (ok && length >= 0 && static_cast<int64_t>(data->size()) < length) {
    *error = url + ": the range ends early";
    ok = false;
  }
  if (ok && length < 0 && !source->url().empty()) {
    url_ = source->url();
  }
  std::lock_guard<std::mutex> lock(fetch_mutex_);
  fetching_ = nullptr;
  return ok;
}

bool DashDemuxer::Open(const std::string& url, std::string* error) {
  return Open(nullptr, url, error);
}

bool DashDemuxer::Open(std::unique_ptr<DataSource> source,
                       const std::string& url,
                       std::string* error) {
  url_ = url;
  std::string text;
  if (source) {
    // The probe's source: read where it is, as a fetch would.
    {
      std::lock_guard<std::mutex> lock(fetch_mutex_);
      fetching_ = source.get();
      if (aborted_) {
        source->Interrupt();
      }
    }
    uint8_t chunk[16384];
    bool failed = false;
    for (;;) {
      const int64_t n =
          source->Read(static_cast<int64_t>(text.size()), chunk, sizeof(chunk));
      if (n < 0) {
        *error = url + ": " + source->error();
        failed = true;
        break;
      }
      if (n == 0) {
        break;
      }
      text.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(n));
      if (static_cast<int64_t>(text.size()) > kMaxManifestBytes) {
        *error = url + ": manifest too large";
        failed = true;
        break;
      }
    }
    if (!source->url().empty()) {
      url_ = source->url();
    }
    {
      std::lock_guard<std::mutex> lock(fetch_mutex_);
      fetching_ = nullptr;
    }
    if (failed) {
      return false;
    }
  } else if (!Fetch(url, -1, -1, &text, error)) {
    return false;
  }
  if (!ParseMpd(text, url_, &manifest_, error)) {
    return false;
  }
  if (manifest_.dynamic) {
    *error = "live (dynamic) DASH is not supported";
    return false;
  }
  if (manifest_.periods > 1) {
    LOGW("%zu periods: only the first plays", manifest_.periods);
  }

  // The master playlist: video Representations as variants, the other
  // AdaptationSets as renditions.
  master_ = {};
  master_.master = true;
  bool has_audio = false;
  bool has_text = false;
  int audio_variant_set = -1;
  for (const DashAdaptationSet& set : manifest_.adaptation_sets) {
    has_audio = has_audio || set.type == DashAdaptationSet::Type::kAudio;
    has_text = has_text || set.type == DashAdaptationSet::Type::kText;
  }
  auto name = [this](size_t set, size_t representation) {
    Named named;
    named.url = url_ + "#dash-" + std::to_string(set) + "-" +
                std::to_string(representation);
    named.set = set;
    named.representation = representation;
    named_.push_back(named);
    return named.url;
  };
  // An audio AdaptationSet plays its highest bandwidth.
  auto best = [](const DashAdaptationSet& set) {
    size_t pick = 0;
    for (size_t i = 1; i < set.representations.size(); ++i) {
      if (set.representations[i].bandwidth >
          set.representations[pick].bandwidth) {
        pick = i;
      }
    }
    return pick;
  };
  for (size_t s = 0; s < manifest_.adaptation_sets.size(); ++s) {
    const DashAdaptationSet& set = manifest_.adaptation_sets[s];
    if (set.type != DashAdaptationSet::Type::kVideo) {
      continue;
    }
    for (size_t r = 0; r < set.representations.size(); ++r) {
      const DashRepresentation& rep = set.representations[r];
      HlsVariant variant;
      variant.url = name(s, r);
      variant.bandwidth = rep.bandwidth;
      variant.width = rep.width;
      variant.height = rep.height;
      variant.frame_rate = rep.frame_rate;
      variant.codecs = rep.codecs;
      variant.audio = has_audio ? "audio" : "";
      variant.subtitles = has_text ? "text" : "";
      master_.variants.push_back(std::move(variant));
    }
  }
  if (master_.variants.empty()) {
    // Audio only: the first audio AdaptationSet is the variant.
    for (size_t s = 0; s < manifest_.adaptation_sets.size(); ++s) {
      const DashAdaptationSet& set = manifest_.adaptation_sets[s];
      if (set.type == DashAdaptationSet::Type::kAudio) {
        HlsVariant variant;
        const size_t r = best(set);
        variant.url = name(s, r);
        variant.bandwidth = set.representations[r].bandwidth;
        variant.audio = "audio";
        variant.subtitles = has_text ? "text" : "";
        master_.variants.push_back(std::move(variant));
        audio_variant_set = static_cast<int>(s);
        break;
      }
    }
  }
  for (size_t s = 0; s < manifest_.adaptation_sets.size(); ++s) {
    const DashAdaptationSet& set = manifest_.adaptation_sets[s];
    if (static_cast<int>(s) == audio_variant_set) {
      continue;
    }
    HlsRendition rendition;
    if (set.type == DashAdaptationSet::Type::kAudio) {
      rendition.type = HlsRendition::Type::kAudio;
      rendition.group = "audio";
      rendition.is_default = set.main;
      rendition.url = name(s, best(set));
    } else if (set.type == DashAdaptationSet::Type::kText) {
      rendition.type = HlsRendition::Type::kSubtitles;
      rendition.group = "text";
      rendition.forced = set.forced;
      rendition.url = name(s, 0);
    } else {
      continue;
    }
    rendition.name = set.label;
    rendition.language = set.language;
    rendition.role = set.role;
    master_.renditions.push_back(std::move(rendition));
  }
  if (master_.variants.empty()) {
    *error = "DASH without video or audio";
    return false;
  }
  LOGI("%zu adaptation sets, %zu variants, %zu renditions",
       manifest_.adaptation_sets.size(), master_.variants.size(),
       master_.renditions.size());
  return HlsDemuxer::Open(nullptr, url_, error);
}

bool DashDemuxer::Playlist(const std::string& url,
                           HlsPlaylist* playlist,
                           std::string* error) {
  if (url == url_) {
    *playlist = master_;
    return true;
  }
  const auto named =
      std::find_if(named_.begin(), named_.end(),
                   [&url](const Named& known) { return known.url == url; });
  if (named == named_.end()) {
    *error = url + ": not a Representation";
    return false;
  }
  const DashAdaptationSet& set = manifest_.adaptation_sets[named->set];
  const DashRepresentation& rep = set.representations[named->representation];
  std::vector<DashSubsegment> index;
  const DashSegments& segments = rep.segments;
  if (segments.kind == DashSegments::Kind::kBase) {
    if (segments.index_offset < 0 || segments.index_length <= 0 ||
        segments.index_length > kMaxIndexBytes) {
      *error = rep.base_url + ": SegmentBase without a usable indexRange";
      return false;
    }
    std::string data;
    if (!Fetch(rep.base_url, segments.index_offset, segments.index_length,
               &data, error)) {
      return false;
    }
    if (!ParseSidx(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
                   segments.index_offset, &index)) {
      *error = rep.base_url + ": no segment index ('sidx') at indexRange";
      return false;
    }
  }
  if (!DashMediaPlaylist(rep, set.type, manifest_.duration, index, playlist,
                         error)) {
    *error = "Representation " + rep.id + ": " + *error;
    return false;
  }
  return true;
}

void DashDemuxer::Interrupt() {
  {
    std::lock_guard<std::mutex> lock(fetch_mutex_);
    if (fetching_) {
      fetching_->Interrupt();
    }
  }
  HlsDemuxer::Interrupt();
}

void DashDemuxer::Abort() {
  {
    std::lock_guard<std::mutex> lock(fetch_mutex_);
    aborted_ = true;
    if (fetching_) {
      fetching_->Interrupt();
    }
  }
  HlsDemuxer::Abort();
}

}  // namespace lychee
