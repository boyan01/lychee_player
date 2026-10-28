#define LOG_TAG "hls"

#include "backends/hls/hls_demuxer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include "base/log.h"
#include "base/time.h"

#if defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#elif defined(_WIN32)
#include <windows.h>

#include <bcrypt.h>
#endif

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
// Audio and subtitle renditions of a variant read at most, and other
// variants listed.
constexpr size_t kMaxAudioRenditions = 8;
constexpr size_t kMaxSubtitleRenditions = 8;
constexpr size_t kMaxVariants = 16;
// About what the player's queues hold ahead of the clock: a rendition
// enabled while playing starts this much before the read position, so
// that it covers what plays next (what is older is dropped).
constexpr int64_t kReadAhead = 5 * kMicrosPerSecond;
// Segments read whole (encrypted, fMP4) are at most this large.
constexpr int64_t kMaxSegmentBytes = int64_t{256} << 20;
// A variant is played if the estimate is at least this much above its
// bandwidth (in percent), for the times the network slows down.
constexpr int64_t kBandwidthHeadroom = 133;
// Smaller segments measure latency more than bandwidth.
constexpr int64_t kMinMeasuredBytes = 64 << 10;

}  // namespace

HlsDemuxer::HlsDemuxer() = default;

HlsDemuxer::~HlsDemuxer() {
  for (auto& r : renditions_) {
    CloseSegment(r.get());
  }
}

// static
bool HlsDemuxer::Detect(const uint8_t* data, size_t size) {
  if (size >= 3 && memcmp(data, "\xEF\xBB\xBF", 3) == 0) {
    data += 3;
    size -= 3;
  }
  return size >= 7 && memcmp(data, "#EXTM3U", 7) == 0;
}

int64_t HlsDemuxer::Now() const {
  return clock_.now ? clock_.now() : MonotonicMicros();
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
    source = opener_(url, error);
    if (!source) {
      return false;
    }
  }
  Track(source.get());
  std::string text;
  uint8_t chunk[16384];
  bool ok = true;
  for (;;) {
    int64_t n =
        source->Read(static_cast<int64_t>(text.size()), chunk, sizeof(chunk));
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
  Track(nullptr);
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
  auto main = std::make_unique<Rendition>();
  main->url = url;
  std::vector<HlsRendition> audio;
  std::vector<HlsRendition> subtitles;
  std::vector<HlsVariant> variants;
  int pick = -1;
  if (playlist.master) {
    pick = PickHlsVariant(playlist.variants);
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
    main->url = variant.url;
    for (const HlsRendition& rendition : playlist.renditions) {
      if (rendition.type == HlsRendition::Type::kAudio &&
          !variant.audio.empty() && rendition.group == variant.audio &&
          !rendition.url.empty() && audio.size() < kMaxAudioRenditions) {
        audio.push_back(rendition);
      }
      if (rendition.type == HlsRendition::Type::kSubtitles &&
          !variant.subtitles.empty() && rendition.group == variant.subtitles &&
          !rendition.url.empty() && subtitles.size() < kMaxSubtitleRenditions) {
        subtitles.push_back(rendition);
      }
    }
    variants = playlist.variants;
    if (!LoadPlaylist(nullptr, main->url, &playlist, &final_url, error)) {
      return false;
    }
    if (playlist.master) {
      *error = "HLS variant is a master playlist";
      return false;
    }
  }
  renditions_.push_back(std::move(main));
  if (!Start(renditions_[0].get(), playlist, error)) {
    return false;
  }
  const bool had_audio = media_info_.default_audio_stream >= 0;
  int default_audio = -1;
  bool default_is_tagged = false;
  for (const HlsRendition& info : audio) {
    auto r = std::make_unique<Rendition>();
    r->url = info.url;
    r->only = MediaType::kAudio;
    HlsPlaylist list;
    std::string why;
    if (!LoadPlaylist(nullptr, r->url, &list, &final_url, &why) ||
        list.master || !Start(r.get(), list, &why)) {
      CloseSegment(r.get());
      if (Failed()) {
        *error = "aborted";
        return false;
      }
      LOGW("audio rendition \"%s\": %s", info.name.c_str(),
           list.master ? "a master playlist" : why.c_str());
      continue;
    }
    LOGI("audio rendition \"%s\" (%s)%s", info.name.c_str(),
         info.language.c_str(), info.is_default ? ", default" : "");
    for (int index : r->streams) {
      StreamInfo& stream = media_info_.streams[index];
      if (!info.language.empty()) {
        stream.language = info.language;
      }
      stream.title = info.name;
      stream.default_track = info.is_default;
      // The DEFAULT=YES one, else the first if the variant has none.
      if (info.is_default ? !default_is_tagged
                          : default_audio < 0 && !had_audio) {
        default_audio = index;
        default_is_tagged = info.is_default;
      }
    }
    renditions_.push_back(std::move(r));
  }
  if (default_audio >= 0) {
    media_info_.default_audio_stream = default_audio;
  }
  for (const HlsRendition& info : subtitles) {
    auto r = std::make_unique<Rendition>();
    r->url = info.url;
    r->only = MediaType::kSubtitle;
    HlsPlaylist list;
    std::string why;
    if (!LoadPlaylist(nullptr, r->url, &list, &final_url, &why) ||
        list.master || !Start(r.get(), list, &why)) {
      CloseSegment(r.get());
      if (Failed()) {
        *error = "aborted";
        return false;
      }
      if (refuse_subtitles_) {
        *error = "subtitle rendition \"" + info.name + "\": " + why;
        return false;
      }
      LOGW("subtitle rendition \"%s\": %s", info.name.c_str(),
           list.master ? "a master playlist" : why.c_str());
      continue;
    }
    for (int index : r->streams) {
      StreamInfo& stream = media_info_.streams[index];
      stream.language = info.language;
      stream.title = info.name;
      stream.default_track = info.is_default;
      if (info.is_default && media_info_.default_subtitle_stream < 0) {
        media_info_.default_subtitle_stream = index;
      }
    }
    renditions_.push_back(std::move(r));
  }
  if (pick >= 0) {
    AddVariants(variants, pick);
  }
  const Rendition& variant = *renditions_[0];
  media_info_.format_name = "hls";
  media_info_.seekable = !variant.live;
  if (!variant.live) {
    const Segment& last = variant.segments.back();
    media_info_.duration = last.start + last.duration;
  }
  enabled_.assign(media_info_.streams.size(), true);
  need_keyframe_.assign(media_info_.streams.size(), true);
  delivered_.assign(media_info_.streams.size(), kNoTimestamp);
  return true;
}

bool HlsDemuxer::Start(Rendition* r,
                       const HlsPlaylist& playlist,
                       std::string* error) {
  r->live = !playlist.ended;
  r->ended = playlist.ended;
  r->target_duration = std::max(playlist.target_duration, kMicrosPerSecond);
  Merge(r, playlist);
  r->last_reload = Now();
  if (r->segments.empty() && !r->live) {
    *error = "HLS playlist without segments";
    return false;
  }
  if (r->live && !r->segments.empty()) {
    // The last segment that starts at least three target durations
    // before the end.
    const Segment& last = r->segments.back();
    const int64_t end = last.start + last.duration;
    r->current = 0;
    for (size_t i = r->segments.size(); i-- > 0;) {
      if (end - r->segments[i].start >=
          kLiveStartTargetDurations * r->target_duration) {
        r->current = i;
        break;
      }
    }
  }
  if (r->live && r->current >= r->segments.size()) {
    DemuxerReadResult result = WaitForSegments(r);
    if (result != DemuxerReadResult::kOk) {
      *error = result == DemuxerReadResult::kError ? error_ : "aborted";
      return false;
    }
  }
  if (!OpenSegment(r, error)) {
    CloseSegment(r);
    return false;
  }
  r->loaded = true;
  for (int index : r->streams) {
    if (r->only == MediaType::kUnknown && r->video < 0 &&
        media_info_.streams[index].type == MediaType::kVideo) {
      r->video = index;
    }
  }
  return true;
}

void HlsDemuxer::Load(Rendition* r, const HlsPlaylist& playlist) {
  r->live = !playlist.ended;
  r->ended = playlist.ended;
  r->target_duration = std::max(playlist.target_duration, kMicrosPerSecond);
  Merge(r, playlist);
  r->last_reload = Now();
  r->loaded = true;
  if (r->live) {
    // Variants of a live stream number their segments alike: on from the
    // first variant's at r->resume (else the one it reads), else three
    // target durations from the end.
    const Rendition& first = *renditions_[0];
    int64_t sequence = first.current < first.segments.size()
                           ? first.segments[first.current].sequence
                       : first.segments.empty()
                           ? -1
                           : first.segments.back().sequence + 1;
    if (r->resume != kNoTimestamp && !first.segments.empty()) {
      const int64_t position =
          r->resume - media_info_.start_time + first.origin;
      sequence = first.segments.front().sequence;
      for (const Segment& segment : first.segments) {
        if (segment.start <= position) {
          sequence = segment.sequence;
        }
      }
    }
    r->current = 0;
    bool found = false;
    for (size_t i = 0; i < r->segments.size() && !found; ++i) {
      if (r->segments[i].sequence == sequence) {
        r->current = i;
        found = true;
      }
    }
    if (!found && !r->segments.empty()) {
      const Segment& last = r->segments.back();
      const int64_t end = last.start + last.duration;
      for (size_t i = r->segments.size(); i-- > 0;) {
        if (end - r->segments[i].start >=
            kLiveStartTargetDurations * r->target_duration) {
          r->current = i;
          break;
        }
      }
    }
    r->resume = kNoTimestamp;
  } else if (r->resume != kNoTimestamp) {
    const int64_t time = r->resume;
    r->resume = kNoTimestamp;
    Position(r, time);
  }
}

void HlsDemuxer::AddVariants(const std::vector<HlsVariant>& variants,
                             int pick) {
  const Rendition& first = *renditions_[0];
  if (first.video < 0) {
    return;
  }
  const VideoCodec played = media_info_.streams[first.video].video.codec;
  // Their audio is the first variant's: the same, muxed alike.
  std::vector<int> audio;
  for (int index : first.streams) {
    if (media_info_.streams[index].type == MediaType::kAudio) {
      audio.push_back(index);
    }
  }
  auto describe = [](const HlsVariant& variant) {
    char title[64];
    if (variant.width > 0 && variant.height > 0) {
      snprintf(title, sizeof(title), "%dx%d, %lld kbit/s", variant.width,
               variant.height,
               static_cast<long long>(variant.bandwidth / 1000));
    } else {
      snprintf(title, sizeof(title), "%lld kbit/s",
               static_cast<long long>(variant.bandwidth / 1000));
    }
    return std::string(title);
  };
  media_info_.streams[first.video].title = describe(variants[pick]);
  // The ladder: decodable variants of the played codec and the same
  // audio, one per URL.
  const VideoCodec codec_played = played;
  auto codec_of = [codec_played](const HlsVariant& variant) {
    const std::string& codecs = variant.codecs;
    if (codecs.empty()) {
      return codec_played;
    }
    return codecs.find("avc1") != std::string::npos ||
                   codecs.find("avc3") != std::string::npos
               ? VideoCodec::kH264
           : codecs.find("hvc1") != std::string::npos ||
                   codecs.find("hev1") != std::string::npos
               ? VideoCodec::kHEVC
               : VideoCodec::kUnknown;
  };
  for (const HlsVariant& variant : variants) {
    if (HlsCodecsDecodable(variant.codecs) &&
        codec_of(variant) == codec_played &&
        variant.audio == variants[pick].audio &&
        std::none_of(ladder_.begin(), ladder_.end(),
                     [&variant](const HlsVariant& known) {
                       return known.url == variant.url;
                     })) {
      ladder_.push_back(variant);
    }
  }
  std::stable_sort(ladder_.begin(), ladder_.end(),
                   [](const HlsVariant& a, const HlsVariant& b) {
                     return a.bandwidth < b.bandwidth;
                   });
  for (size_t i = 0; i < ladder_.size(); ++i) {
    if (ladder_[i].url == variants[pick].url) {
      rung_ = i;
    }
  }
  std::vector<std::string> urls = {variants[pick].url};
  for (const HlsVariant& variant : variants) {
    if (urls.size() > kMaxVariants ||
        std::find(urls.begin(), urls.end(), variant.url) != urls.end() ||
        !HlsCodecsDecodable(variant.codecs)) {
      continue;
    }
    const VideoCodec codec = codec_of(variant);
    if (codec == VideoCodec::kUnknown) {
      continue;  // Audio only.
    }
    urls.push_back(variant.url);
    StreamInfo info;
    info.index = static_cast<int>(media_info_.streams.size());
    info.type = MediaType::kVideo;
    info.title = describe(variant);
    info.video.codec = codec;
    info.video.codec_name = VideoCodecName(codec);
    info.video.width = variant.width;
    info.video.height = variant.height;
    media_info_.streams.push_back(info);
    auto r = std::make_unique<Rendition>();
    r->url = variant.url;
    r->video = info.index;
    r->streams.push_back(info.index);
    r->streams.insert(r->streams.end(), audio.begin(), audio.end());
    renditions_.push_back(std::move(r));
  }
}

bool HlsDemuxer::Merge(Rendition* r, const HlsPlaylist& playlist) {
  std::vector<Segment>& segments = r->segments;
  bool added = false;
  for (const HlsSegment& segment : playlist.segments) {
    if (!segments.empty() && segment.sequence <= segments.back().sequence) {
      continue;
    }
    Segment entry;
    static_cast<HlsSegment&>(entry) = segment;
    if (!segments.empty()) {
      entry.start = segments.back().start + segments.back().duration;
    }
    segments.push_back(std::move(entry));
    added = true;
  }
  // Fallen behind a live playlist: what is gone from it cannot be read.
  if (!playlist.segments.empty() && r->current < segments.size() &&
      segments[r->current].sequence < playlist.segments.front().sequence &&
      !r->reader && !r->mp4) {
    while (r->current + 1 < segments.size() &&
           segments[r->current].sequence < playlist.segments.front().sequence) {
      ++r->current;
    }
    LOGW("fell behind the live playlist; skipping to segment %lld",
         static_cast<long long>(segments[r->current].sequence));
  }
  // Live: the segments long played are not needed again.
  if (r->current > 2 * kKeptSegments) {
    const size_t drop = r->current - kKeptSegments;
    segments.erase(segments.begin(),
                   segments.begin() + static_cast<ptrdiff_t>(drop));
    r->current -= drop;
  }
  return added;
}

bool HlsDemuxer::Fetch(const std::string& url,
                       int64_t offset,
                       int64_t length,
                       std::vector<uint8_t>* data,
                       std::string* error,
                       int64_t* rate) {
  std::unique_ptr<DataSource> source = opener_(url, error);
  if (!source) {
    return false;
  }
  Track(source.get());
  const int64_t begin = std::max<int64_t>(offset, 0);
  const int64_t limit =
      length >= 0 ? std::min(length, kMaxSegmentBytes) : kMaxSegmentBytes;
  data->clear();
  bool ok = true;
  while (static_cast<int64_t>(data->size()) < limit) {
    const size_t old = data->size();
    const size_t want = static_cast<size_t>(
        std::min<int64_t>(limit - static_cast<int64_t>(old), 256 << 10));
    data->resize(old + want);
    const int64_t n = source->Read(begin + static_cast<int64_t>(old),
                                   data->data() + old, want);
    data->resize(old + static_cast<size_t>(std::max<int64_t>(n, 0)));
    if (n < 0) {
      *error = url + ": " + source->error();
      ok = false;
      break;
    }
    if (n == 0) {
      break;
    }
  }
  if (ok && length < 0 && static_cast<int64_t>(data->size()) >= limit) {
    *error = url + ": segment too large";
    ok = false;
  }
  if (ok && length >= 0 && static_cast<int64_t>(data->size()) < length) {
    *error = url + ": byte range past the end";
    ok = false;
  }
  if (rate) {
    *rate = source->transfer_rate();
  }
  Track(nullptr);
  return ok;
}

bool HlsDemuxer::Decrypt(const HlsKey& key,
                         int64_t sequence,
                         [[maybe_unused]] std::vector<uint8_t>* data,
                         std::string* error) {
  auto known = keys_.find(key.url);
  if (known == keys_.end()) {
    std::vector<uint8_t> bytes;
    if (!Fetch(key.url, -1, -1, &bytes, error)) {
      return false;
    }
    if (bytes.size() != 16) {
      *error = key.url + ": an AES-128 key is 16 bytes, not " +
               std::to_string(bytes.size());
      return false;
    }
    std::array<uint8_t, 16> value{};
    std::copy(bytes.begin(), bytes.end(), value.begin());
    known = keys_.emplace(key.url, value).first;
  }
  uint8_t iv[16];
  HlsIv(key, sequence, iv);
#if defined(__APPLE__)
  std::vector<uint8_t> plain(data->size());
  size_t moved = 0;
  const CCCryptorStatus status =
      CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding,
              known->second.data(), known->second.size(), iv, data->data(),
              data->size(), plain.data(), plain.size(), &moved);
  if (status != kCCSuccess) {
    *error = "AES-128 decryption failed (" + std::to_string(status) + ")";
    return false;
  }
  plain.resize(moved);
  *data = std::move(plain);
  return true;
#elif defined(_WIN32)
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_KEY_HANDLE handle = nullptr;
  NTSTATUS status =
      BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
  ULONG moved = 0;
  std::vector<uint8_t> plain(data->size());
  if (BCRYPT_SUCCESS(status)) {
    status = BCryptSetProperty(
        algorithm, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
        sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
  }
  if (BCRYPT_SUCCESS(status)) {
    status = BCryptGenerateSymmetricKey(
        algorithm, &handle, nullptr, 0, known->second.data(),
        static_cast<ULONG>(known->second.size()), 0);
  }
  if (BCRYPT_SUCCESS(status)) {
    status = BCryptDecrypt(
        handle, data->data(), static_cast<ULONG>(data->size()), nullptr, iv,
        sizeof(iv), plain.data(), static_cast<ULONG>(plain.size()), &moved,
        BCRYPT_BLOCK_PADDING);
  }
  if (handle) {
    BCryptDestroyKey(handle);
  }
  if (algorithm) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
  }
  if (!BCRYPT_SUCCESS(status)) {
    char code[16];
    snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(status));
    *error = std::string("AES-128 decryption failed (") + code + ")";
    return false;
  }
  plain.resize(moved);
  *data = std::move(plain);
  return true;
#else
  *error = "AES-128 HLS needs FFmpeg here";
  return false;
#endif
}

bool HlsDemuxer::LoadSegment(Rendition* r,
                             const Segment& segment,
                             std::vector<uint8_t>* data,
                             std::string* error) {
  data->clear();
  if (segment.initialization) {
    const HlsMap& map = segment.map;
    const std::string id = map.url + "@" + std::to_string(map.offset) + "+" +
                           std::to_string(map.length);
    if (id != r->map_id) {
      r->map_id.clear();
      if (!Fetch(map.url, map.offset, map.length, &r->map_data, error) ||
          (map.key.method == HlsKey::Method::kAes128 &&
           !Decrypt(map.key, segment.sequence, &r->map_data, error))) {
        return false;
      }
      r->map_id = id;
    }
    *data = r->map_data;
  }
  std::vector<uint8_t> media;
  int64_t rate = -1;
  if (!Fetch(segment.url, segment.offset, segment.length, &media, error,
             &rate) ||
      (segment.key.method == HlsKey::Method::kAes128 &&
       !Decrypt(segment.key, segment.sequence, &media, error))) {
    return false;
  }
  if (r == renditions_[0].get()) {
    Measure(rate, static_cast<int64_t>(media.size()));
  }
  data->insert(data->end(), media.begin(), media.end());
  return true;
}

void HlsDemuxer::Measure(int64_t rate, int64_t bytes) {
  if (rate <= 0 || bytes < kMinMeasuredBytes) {
    return;
  }
  // Halfway to each new segment's: follows a change within a few.
  estimate_ = estimate_ > 0 ? (estimate_ + rate) / 2 : rate;
}

void HlsDemuxer::Adapt(Rendition* r) {
  if (ladder_.size() < 2 || estimate_ <= 0 || r->video < 0 ||
      !enabled_[r->video] || r->current >= r->segments.size() ||
      r->segments[r->current].initialization) {
    return;
  }
  size_t want = 0;
  for (size_t i = 1; i < ladder_.size(); ++i) {
    if (ladder_[i].bandwidth * kBandwidthHeadroom <= estimate_ * 100) {
      want = i;
    }
  }
  if (want == rung_) {
    return;
  }
  const HlsVariant& variant = ladder_[want];
  HlsPlaylist playlist;
  std::string final_url;
  std::string error;
  if (!LoadPlaylist(nullptr, variant.url, &playlist, &final_url, &error) ||
      playlist.master || playlist.segments.empty() ||
      std::any_of(playlist.segments.begin(), playlist.segments.end(),
                  [](const HlsSegment& s) { return s.initialization; })) {
    LOGW("variant %s: %s", variant.url.c_str(),
         error.empty() ? "not MPEG-TS" : error.c_str());
    ladder_.erase(ladder_.begin() + static_cast<ptrdiff_t>(want));
    if (want < rung_) {
      --rung_;
    }
    return;
  }
  Rendition next;
  Merge(&next, playlist);
  // Where r goes on: the segment that holds the next one's start (VOD),
  // or has its number (live), on r's playlist time.
  const Segment& from = r->segments[r->current];
  size_t index = next.segments.size();
  for (size_t i = 0; i < next.segments.size(); ++i) {
    const Segment& segment = next.segments[i];
    if (r->live ? segment.sequence == from.sequence
                : segment.start <= from.start + from.duration / 2) {
      index = i;
    }
  }
  if (index == next.segments.size()) {
    return;  // Not in its window yet: on the next segment.
  }
  const int64_t delta = r->live ? from.start - next.segments[index].start : 0;
  for (Segment& segment : next.segments) {
    segment.start += delta;
  }
  LOGI("variant %s (%lld bit/s): %lld bit/s measured", variant.url.c_str(),
       static_cast<long long>(variant.bandwidth),
       static_cast<long long>(estimate_));
  r->url = variant.url;
  r->segments = std::move(next.segments);
  r->current = index;
  r->live = !playlist.ended;
  r->ended = playlist.ended;
  r->target_duration = std::max(playlist.target_duration, kMicrosPerSecond);
  r->last_reload = Now();
  rung_ = want;
}

bool HlsDemuxer::OpenWebVtt(Rendition* r, std::string* error) {
  const Segment& segment = r->segments[r->current];
  std::vector<uint8_t> data;
  if (!LoadSegment(r, segment, &data, error)) {
    return false;
  }
  int64_t mpegts = 0;
  std::vector<WebVttCue> cues;
  if (!ParseHlsWebVtt(std::string(data.begin(), data.end()), &mpegts, &cues)) {
    *error = segment.url + ": not WebVTT";
    return false;
  }
  if (r->streams.empty()) {
    StreamInfo info;
    info.index = static_cast<int>(media_info_.streams.size());
    info.type = MediaType::kSubtitle;
    info.subtitle.codec_name = "webvtt";
    info.subtitle.text = true;
    media_info_.streams.push_back(info);
    r->streams.push_back(info.index);
  }
  r->stream_map = {r->streams[0]};
  if (!r->opened) {
    // Its playlist times are the first variant's.
    r->origin = renditions_[0]->origin;
    r->opened = true;
  }
  auto anchor = offsets_.find(segment.discontinuity);
  r->offset = anchor != offsets_.end() ? anchor->second : 0;
  // The MPEG-2 timestamp unwrapped next to where the segment plays.
  const int64_t near =
      media_info_.start_time + segment.start - r->origin - r->offset;
  const int64_t base = TsReader::TicksToMicros(
      TsReader::Nearest(mpegts, TsReader::MicrosToTicks(near)));
  for (const WebVttCue& cue : cues) {
    EncodedPacket packet;
    packet.stream_index = 0;
    packet.pts = packet.dts = base + cue.start;
    packet.duration = cue.end - cue.start;
    packet.keyframe = true;
    packet.buffer = PacketBuffer::Copy(
        reinterpret_cast<const uint8_t*>(cue.text.data()), cue.text.size());
    r->text.push_back(std::move(packet));
  }
  // In decode order with the others.
  std::stable_sort(r->text.begin(), r->text.end(),
                   [](const EncodedPacket& a, const EncodedPacket& b) {
                     return a.dts < b.dts;
                   });
  r->text_open = true;
  return true;
}

bool HlsDemuxer::OpenSegment(Rendition* r, std::string* error) {
  if (r->only == MediaType::kSubtitle) {
    return OpenWebVtt(r, error);
  }
  const Segment& segment = r->segments[r->current];
  if (segment.key.method == HlsKey::Method::kOther ||
      (segment.initialization &&
       segment.map.key.method == HlsKey::Method::kOther)) {
    *error = "HLS encryption other than AES-128 (EXT-X-KEY) needs FFmpeg";
    return false;
  }
  auto anchor = offsets_.find(segment.discontinuity);
  // The segment's streams and the first timestamp of each.
  std::vector<std::pair<StreamInfo, int64_t>> streams;
  std::unique_ptr<TsReader> reader;
  std::unique_ptr<Mp4Demuxer> mp4;
  const bool whole =
      segment.initialization || segment.key.method == HlsKey::Method::kAes128;
  std::unique_ptr<DataSource> source;
  if (whole) {
    std::vector<uint8_t> data;
    if (!LoadSegment(r, segment, &data, error)) {
      return false;
    }
    source = CreateMemoryDataSource(std::move(data));
  } else {
    source = opener_(segment.url, error);
    if (!source) {
      return false;
    }
  }
  if (segment.initialization) {
    mp4 = std::make_unique<Mp4Demuxer>();
    if (!mp4->Open(std::move(source), error)) {
      *error = segment.url + ": " + *error;
      return false;
    }
    for (const StreamInfo& info : mp4->media_info().streams) {
      streams.emplace_back(info, mp4->media_info().start_time);
    }
  } else {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      r->source = std::move(source);
    }
    Track(r->source.get());
    reader = std::make_unique<TsReader>(r->source.get());
    const int64_t begin = !whole && segment.offset >= 0 ? segment.offset : 0;
    reader->SetRange(
        begin, !whole && segment.length >= 0 ? begin + segment.length : -1);
    // Timestamps unwrap next to where the segment should start.
    reader->Restart(begin, anchor == offsets_.end() || !r->opened
                               ? kNoTimestamp
                               : media_info_.start_time + segment.start -
                                     r->origin - anchor->second);
    if (!reader->Probe(kSegmentProbeBytes, error)) {
      *error = segment.url + ": " + *error;
      return false;
    }
    for (const TsStream& stream : reader->streams()) {
      streams.emplace_back(stream.info, stream.first_pts);
    }
  }

  if (r->streams.empty()) {
    for (const auto& [info, first_pts] : streams) {
      if (r->only != MediaType::kUnknown && info.type != r->only) {
        continue;
      }
      const int index = static_cast<int>(media_info_.streams.size());
      media_info_.streams.push_back(info);
      media_info_.streams.back().index = index;
      r->streams.push_back(index);
      if (info.type == MediaType::kVideo &&
          media_info_.default_video_stream < 0) {
        media_info_.default_video_stream = index;
      }
      if (info.type == MediaType::kAudio &&
          media_info_.default_audio_stream < 0 && renditions_[0].get() == r) {
        media_info_.default_audio_stream = index;
      }
    }
    if (r->streams.empty()) {
      *error = "no H.264, HEVC or audio stream in the HLS segment";
      return false;
    }
  }
  // A segment's n-th video (audio) stream is the rendition's n-th.
  r->stream_map.clear();
  int annexb_stream = -1;
  int64_t start = kNoTimestamp;
  for (size_t i = 0; i < streams.size(); ++i) {
    const StreamInfo& stream = streams[i].first;
    int ordinal = 0;
    for (size_t k = 0; k < i; ++k) {
      if (streams[k].first.type == stream.type) {
        ++ordinal;
      }
    }
    int index = -1;
    for (int candidate : r->streams) {
      if (media_info_.streams[candidate].type == stream.type &&
          ordinal-- == 0) {
        index = candidate;
        break;
      }
    }
    r->stream_map.push_back(index);
    if (mp4 && index >= 0 && stream.type == MediaType::kVideo &&
        media_info_.streams[index].video.extradata.empty() &&
        !stream.video.extradata.empty() && r->annexb.Initialize(stream.video)) {
      annexb_stream = static_cast<int>(i);
    }
    const int64_t first_pts = streams[i].second;
    if (index >= 0 && first_pts != kNoTimestamp &&
        (start == kNoTimestamp || first_pts < start)) {
      start = first_pts;
    }
  }
  if (anchor == offsets_.end()) {
    if (start == kNoTimestamp) {
      start = 0;
    }
    if (offsets_.empty()) {
      media_info_.start_time = start;
    }
    if (!r->opened) {
      r->origin = segment.start;
    }
    r->offset = media_info_.start_time + segment.start - r->origin - start;
    offsets_[segment.discontinuity] = r->offset;
  } else {
    r->offset = anchor->second;
    if (!r->opened) {
      // Another rendition placed the discontinuity: this playlist's times
      // follow from where its first segment lands.
      r->origin = segment.start;
      if (start != kNoTimestamp) {
        r->origin -= start + r->offset - media_info_.start_time;
      }
    }
  }
  r->opened = true;
  r->annexb_stream = annexb_stream;
  r->reader = std::move(reader);
  r->mp4 = std::move(mp4);
  return true;
}

void HlsDemuxer::CloseSegment(Rendition* r) {
  r->reader.reset();
  r->mp4.reset();
  r->text_open = false;
  r->text.clear();
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_ == r->source.get()) {
    active_ = nullptr;
  }
  r->source.reset();
}

DemuxerReadResult HlsDemuxer::WaitForSegments(Rendition* r) {
  while (r->current >= r->segments.size()) {
    if (r->ended) {
      return DemuxerReadResult::kEndOfStream;
    }
    // RFC 8216 6.3.4: a target duration after a reload that changed the
    // playlist, half of one after one that did not.
    const int64_t due =
        r->last_reload +
        (r->last_reload_changed ? r->target_duration : r->target_duration / 2);
    if (clock_.wait) {
      if (!clock_.wait(due)) {
        return DemuxerReadResult::kAborted;
      }
    } else {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(
          lock, std::chrono::microseconds(std::max<int64_t>(due - Now(), 0)),
          [this] { return aborted_ || interrupted_; });
    }
    if (aborted_ || interrupted_) {
      return DemuxerReadResult::kAborted;
    }
    HlsPlaylist playlist;
    std::string final_url;
    std::string error;
    r->last_reload = Now();
    if (!LoadPlaylist(nullptr, r->url, &playlist, &final_url, &error) ||
        playlist.master) {
      if (Failed()) {
        return DemuxerReadResult::kAborted;
      }
      LOGW("playlist reload failed: %s", error.c_str());
      if (++r->reload_failures >= kMaxReloadFailures) {
        error_ =
            error.empty() ? "live playlist became a master playlist" : error;
        return DemuxerReadResult::kError;
      }
      r->last_reload_changed = false;
      continue;
    }
    r->reload_failures = 0;
    r->last_reload_changed = Merge(r, playlist);
    if (playlist.ended) {
      r->ended = true;
    }
  }
  return DemuxerReadResult::kOk;
}

bool HlsDemuxer::Enabled(const Rendition& r) const {
  if (r.video >= 0 && !enabled_[r.video]) {
    // A variant without its video is read only for audio alone, and only
    // the first.
    if (&r != renditions_[0].get() ||
        std::any_of(renditions_.begin(), renditions_.end(),
                    [this](const std::unique_ptr<Rendition>& other) {
                      return other->video >= 0 && enabled_[other->video];
                    })) {
      return false;
    }
  }
  return std::any_of(r.streams.begin(), r.streams.end(),
                     [this](int index) { return enabled_[index]; });
}

void HlsDemuxer::Position(Rendition* r, int64_t time) {
  if (!r->loaded) {
    r->resume = time;
    r->has_next = false;
    r->at_end = false;
    return;
  }
  // The segment that holds |time|; decoding starts at its first keyframe.
  const int64_t position = time - media_info_.start_time + r->origin;
  size_t index = 0;
  for (size_t i = 0; i < r->segments.size(); ++i) {
    if (r->segments[i].start <= position) {
      index = i;
    }
  }
  CloseSegment(r);
  r->current = index;
  r->has_next = false;
  r->at_end = false;
}

void HlsDemuxer::SetEnabledStreams(const std::vector<int>& stream_indices) {
  std::vector<bool> was;
  was.reserve(renditions_.size());
  for (const auto& r : renditions_) {
    was.push_back(Enabled(*r));
  }
  std::fill(enabled_.begin(), enabled_.end(), false);
  for (int index : stream_indices) {
    if (index >= 0 && index < static_cast<int>(enabled_.size())) {
      enabled_[index] = true;
    }
  }
  for (size_t i = 0; i < renditions_.size(); ++i) {
    Rendition* r = renditions_[i].get();
    if (was[i] && !Enabled(*r)) {
      // Not read meanwhile: its connection goes.
      CloseSegment(r);
      r->has_next = false;
    } else if (!was[i] && Enabled(*r) && last_dts_ != kNoTimestamp) {
      // Goes on where the others play (a seek after the track change
      // moves it again).
      Position(r, last_dts_ - kReadAhead);
      r->skip_until = delivered_;
      for (int index : r->streams) {
        need_keyframe_[index] = true;
      }
    }
  }
}

DemuxerReadResult HlsDemuxer::ReadFrom(Rendition* r, EncodedPacket* packet) {
  for (;;) {
    if (aborted_ || interrupted_) {
      return DemuxerReadResult::kAborted;
    }
    if (!r->loaded) {
      HlsPlaylist playlist;
      std::string final_url;
      std::string error;
      if (!LoadPlaylist(nullptr, r->url, &playlist, &final_url, &error) ||
          playlist.master) {
        if (Failed()) {
          return DemuxerReadResult::kAborted;
        }
        error_ = playlist.master ? "HLS variant is a master playlist" : error;
        LOGE("%s", error_.c_str());
        return DemuxerReadResult::kError;
      }
      LOGI("variant %s", r->url.c_str());
      Load(r, playlist);
      continue;
    }
    if (!r->reader && !r->mp4 && !r->text_open) {
      if (r->current >= r->segments.size()) {
        if (!r->live) {
          return DemuxerReadResult::kEndOfStream;
        }
        DemuxerReadResult result = WaitForSegments(r);
        if (result != DemuxerReadResult::kOk) {
          return result;
        }
        continue;
      }
      if (r == renditions_[0].get()) {
        Adapt(r);
      }
      std::string error;
      if (!OpenSegment(r, &error)) {
        const bool failed = Failed();
        CloseSegment(r);
        if (failed) {
          return DemuxerReadResult::kAborted;
        }
        error_ = error;
        LOGE("%s", error_.c_str());
        return DemuxerReadResult::kError;
      }
    }
    Track(r->source.get());
    int64_t offset;
    DemuxerReadResult result = DemuxerReadResult::kEndOfStream;
    if (r->text_open) {
      if (!r->text.empty()) {
        *packet = std::move(r->text.front());
        r->text.pop_front();
        result = DemuxerReadResult::kOk;
      }
    } else {
      result = r->mp4 ? r->mp4->Read(packet) : r->reader->Read(packet, &offset);
    }
    if (result == DemuxerReadResult::kError) {
      if (Failed()) {
        return DemuxerReadResult::kAborted;
      }
      error_ = r->segments[r->current].url + ": " +
               (r->mp4 ? r->mp4->error() : r->reader->error());
      LOGE("%s", error_.c_str());
      return result;
    }
    if (result != DemuxerReadResult::kOk) {
      if (r == renditions_[0].get() && r->reader && r->source) {
        Measure(r->source->transfer_rate(), r->source->size());
      }
      CloseSegment(r);
      ++r->current;
      continue;
    }
    const int source_index = packet->stream_index;
    if (source_index < 0 ||
        source_index >= static_cast<int>(r->stream_map.size())) {
      continue;
    }
    const int index = r->stream_map[source_index];
    if (index < 0 || !enabled_[index]) {
      continue;
    }
    if (media_info_.streams[index].type == MediaType::kVideo) {
      if (need_keyframe_[index] && !packet->keyframe) {
        continue;
      }
      need_keyframe_[index] = false;
    }
    if (source_index == r->annexb_stream && r->mp4 && packet->buffer) {
      std::vector<uint8_t> annexb;
      if (r->annexb.Convert(packet->buffer->data(), packet->buffer->size(),
                            packet->keyframe, &annexb)) {
        packet->buffer = PacketBuffer::Copy(annexb.data(), annexb.size());
      }
    }
    packet->stream_index = index;
    packet->pts += r->offset;
    packet->dts += r->offset;
    return DemuxerReadResult::kOk;
  }
}

DemuxerReadResult HlsDemuxer::Read(EncodedPacket* packet) {
  for (;;) {
    if (aborted_ || interrupted_) {
      return DemuxerReadResult::kAborted;
    }
    // The renditions' next packets, the earliest first.
    Rendition* pick = nullptr;
    for (auto& r : renditions_) {
      if (!Enabled(*r) || r->at_end) {
        continue;
      }
      if (!r->has_next && r->only == MediaType::kSubtitle && !r->text_open &&
          r->loaded && r->current < r->segments.size() &&
          last_dts_ != kNoTimestamp &&
          media_info_.start_time + r->segments[r->current].start - r->origin >
              last_dts_ + kReadAhead) {
        // Subtitle segments may hold no cue for long: not fetched far
        // ahead of the others in search of one.
        continue;
      }
      if (!r->has_next) {
        DemuxerReadResult result = ReadFrom(r.get(), &r->next);
        if (result == DemuxerReadResult::kEndOfStream) {
          r->at_end = true;
          continue;
        }
        if (result != DemuxerReadResult::kOk) {
          return result;
        }
        r->has_next = true;
      }
      if (!pick || r->next.dts < pick->next.dts) {
        pick = r.get();
      }
    }
    if (!pick) {
      return DemuxerReadResult::kEndOfStream;
    }
    pick->has_next = false;
    const int index = pick->next.stream_index;
    if (!enabled_[index]) {
      continue;  // Disabled since it was read ahead.
    }
    if (!pick->skip_until.empty() && pick->skip_until[index] != kNoTimestamp &&
        pick->next.dts <= pick->skip_until[index]) {
      continue;
    }
    *packet = std::move(pick->next);
    last_dts_ = packet->dts;
    delivered_[index] = packet->dts;
    return DemuxerReadResult::kOk;
  }
}

bool HlsDemuxer::Seek(int64_t time) {
  interrupted_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& r : renditions_) {
      if (r->source && !aborted_) {
        r->source->ClearInterrupt();
      }
    }
  }
  if (!media_info_.seekable) {
    return false;
  }
  for (auto& r : renditions_) {
    Position(r.get(), time);
  }
  last_dts_ = kNoTimestamp;
  std::fill(delivered_.begin(), delivered_.end(), kNoTimestamp);
  for (auto& r : renditions_) {
    r->skip_until.clear();
  }
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
