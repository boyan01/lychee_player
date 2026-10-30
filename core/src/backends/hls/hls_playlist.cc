#include "backends/hls/hls_playlist.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "base/time.h"

namespace lychee {

namespace {

// A playlist with more is not one this player can hold: a day of 1 s
// segments is 86400.
constexpr size_t kMaxSegments = 200000;
// Durations are clamped to this many seconds (194 days), so that the
// microseconds of kMaxSegments of them add up without overflow.
constexpr int64_t kMaxSeconds = int64_t{1} << 24;

bool StartsWith(const std::string& text, const char* prefix) {
  return text.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

std::string Trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// A decimal-integer, without locale dependence or overflow (clamped).
int64_t ParseInteger(const std::string& text, size_t* used = nullptr) {
  int64_t value = 0;
  size_t i = 0;
  while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
    if (value < (int64_t{1} << 50)) {
      value = value * 10 + (text[i] - '0');
    }
    ++i;
  }
  if (used) {
    *used = i;
  }
  return value;
}

// A decimal-floating-point number of seconds (EXTINF) in microseconds.
// Written out: strtod follows the locale's decimal separator.
int64_t ParseSeconds(const std::string& text) {
  size_t used = 0;
  int64_t micros =
      std::min(ParseInteger(text, &used), kMaxSeconds) * kMicrosPerSecond;
  if (used < text.size() && text[used] == '.') {
    int64_t scale = kMicrosPerSecond / 10;
    for (size_t i = used + 1;
         i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]));
         ++i) {
      micros += (text[i] - '0') * scale;
      scale /= 10;
    }
  }
  return micros;
}

// An attribute list (RFC 8216 4.2): NAME=VALUE pairs, values quoted or
// not.
std::map<std::string, std::string> ParseAttributes(const std::string& text) {
  std::map<std::string, std::string> attributes;
  size_t i = 0;
  while (i < text.size()) {
    size_t equals = text.find('=', i);
    if (equals == std::string::npos) {
      break;
    }
    std::string name = Trim(text.substr(i, equals - i));
    std::string value;
    i = equals + 1;
    if (i < text.size() && text[i] == '"') {
      size_t close = text.find('"', i + 1);
      if (close == std::string::npos) {
        close = text.size();
      }
      value = text.substr(i + 1, close - i - 1);
      i = text.find(',', close);
    } else {
      size_t comma = text.find(',', i);
      value = Trim(text.substr(
          i, comma == std::string::npos ? std::string::npos : comma - i));
      i = comma;
    }
    attributes[name] = value;
    if (i == std::string::npos) {
      break;
    }
    ++i;
  }
  return attributes;
}

bool HasScheme(const std::string& url) {
  size_t colon = url.find(':');
  // One letter is a Windows drive, not a scheme.
  if (colon == std::string::npos || colon < 2 ||
      !std::isalpha(static_cast<unsigned char>(url[0]))) {
    return false;
  }
  for (size_t i = 1; i < colon; ++i) {
    const char c = url[i];
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' &&
        c != '.') {
      return false;
    }
  }
  return true;
}

// RFC 3986 5.2.4.
std::string RemoveDotSegments(const std::string& path) {
  const bool absolute = !path.empty() && path[0] == '/';
  std::vector<std::string> out;
  bool trailing = false;
  size_t i = absolute ? 1 : 0;
  while (i <= path.size()) {
    size_t slash = path.find('/', i);
    if (slash == std::string::npos) {
      slash = path.size();
    }
    std::string segment = path.substr(i, slash - i);
    const bool last = slash == path.size();
    trailing = false;
    if (segment == ".") {
      trailing = last;
    } else if (segment == "..") {
      if (!out.empty()) {
        out.pop_back();
      }
      trailing = last;
    } else {
      out.push_back(segment);
    }
    i = slash + 1;
  }
  std::string result = absolute ? "/" : "";
  for (size_t k = 0; k < out.size(); ++k) {
    if (k > 0) {
      result += '/';
    }
    result += out[k];
  }
  if (trailing && !out.empty()) {
    result += '/';
  }
  return result;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// A hexadecimal-sequence (0x...) of up to 128 bits, right-aligned in |iv|.
bool ParseIv(const std::string& text, uint8_t iv[16]) {
  if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X') ||
      text.size() - 2 > 32) {
    return false;
  }
  std::fill(iv, iv + 16, uint8_t{0});
  int nibble = 0;
  for (size_t i = text.size(); i-- > 2; ++nibble) {
    const int digit = HexDigit(text[i]);
    if (digit < 0) {
      return false;
    }
    iv[15 - nibble / 2] |= static_cast<uint8_t>(digit << (nibble % 2 * 4));
  }
  return true;
}

HlsKey ParseKey(const std::string& text, const std::string& base_url) {
  auto attributes = ParseAttributes(text);
  HlsKey key;
  const std::string& method = attributes["METHOD"];
  if (method == "NONE") {
    return key;
  }
  key.method =
      method == "AES-128" ? HlsKey::Method::kAes128 : HlsKey::Method::kOther;
  key.url = ResolveUrl(base_url, attributes["URI"]);
  auto iv = attributes.find("IV");
  if (iv != attributes.end()) {
    key.has_iv = ParseIv(iv->second, key.iv);
    if (!key.has_iv) {
      key.method = HlsKey::Method::kOther;  // Not one that can be used.
    }
  }
  return key;
}

// A WebVTT timestamp, [hh:]mm:ss.ttt, in microseconds at |*pos| (moved
// past it); -1 if there is none.
int64_t ParseVttTime(const std::string& text, size_t* pos) {
  int64_t parts[3] = {0, 0, 0};
  int count = 0;
  size_t i = *pos;
  for (;;) {
    size_t digits = 0;
    int64_t value = 0;
    while (i < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[i])) && digits < 10) {
      value = value * 10 + (text[i] - '0');
      ++i;
      ++digits;
    }
    if (digits == 0 || count == 3) {
      return -1;
    }
    parts[count++] = value;
    if (i < text.size() && text[i] == ':') {
      ++i;
      continue;
    }
    break;
  }
  if (count < 2 || i + 4 > text.size() || text[i] != '.') {
    return -1;
  }
  int64_t millis = 0;
  for (size_t k = 1; k <= 3; ++k) {
    if (!std::isdigit(static_cast<unsigned char>(text[i + k]))) {
      return -1;
    }
    millis = millis * 10 + (text[i + k] - '0');
  }
  *pos = i + 4;
  const int64_t hours = count == 3 ? parts[0] : 0;
  const int64_t minutes = parts[count - 2];
  const int64_t seconds = parts[count - 1];
  if (minutes > 59 || seconds > 59 || hours > kMaxSeconds / 3600) {
    return -1;
  }
  return ((hours * 60 + minutes) * 60 + seconds) * kMicrosPerSecond +
         millis * 1000;
}

}  // namespace

void HlsIv(const HlsKey& key, int64_t sequence, uint8_t iv[16]) {
  if (key.has_iv) {
    std::copy(key.iv, key.iv + 16, iv);
    return;
  }
  std::fill(iv, iv + 8, uint8_t{0});
  for (int i = 0; i < 8; ++i) {
    iv[15 - i] =
        static_cast<uint8_t>(static_cast<uint64_t>(sequence) >> (8 * i));
  }
}

std::string ResolveUrl(const std::string& base, const std::string& reference) {
  if (reference.empty()) {
    return base;
  }
  if (HasScheme(reference)) {
    return reference;
  }
  // The reference's path and its query or fragment.
  const size_t reference_tail = reference.find_first_of("?#");
  const std::string reference_path = reference.substr(0, reference_tail);
  const std::string reference_query = reference_tail == std::string::npos
                                          ? ""
                                          : reference.substr(reference_tail);

  std::string prefix;  // scheme://authority
  std::string base_path = base.substr(0, base.find_first_of("?#"));
  if (HasScheme(base)) {
    const size_t colon = base.find(':');
    const std::string scheme = base.substr(0, colon + 1);
    if (StartsWith(reference, "//")) {
      return scheme + reference;
    }
    size_t path_start = colon + 1;
    if (base.compare(path_start, 2, "//") == 0) {
      path_start = base_path.find('/', path_start + 2);
      if (path_start == std::string::npos) {
        path_start = base_path.size();
      }
    }
    prefix = base_path.substr(0, path_start);
    base_path = base_path.substr(path_start);
  }
  if (reference_path.empty()) {
    return prefix + base_path + reference_query;
  }
  std::string path;
  if (reference_path[0] == '/') {
    path = reference_path;
  } else {
    const size_t slash = base_path.rfind('/');
    if (slash != std::string::npos) {
      path = base_path.substr(0, slash + 1) + reference_path;
    } else {
      path = (prefix.empty() ? "" : "/") + reference_path;
    }
  }
  return prefix + RemoveDotSegments(path) + reference_query;
}

bool ParseHlsPlaylist(const std::string& text,
                      const std::string& base_url,
                      HlsPlaylist* playlist,
                      std::string* error) {
  *playlist = HlsPlaylist();
  bool first = true;
  bool pending_variant = false;
  HlsVariant variant;
  int64_t duration = -1;
  int64_t range_length = -1;
  int64_t range_offset = -1;
  int64_t discontinuity_base = 0;
  int64_t discontinuities = 0;
  HlsKey key;
  bool initialization = false;
  HlsMap map;
  size_t i = 0;
  while (i < text.size()) {
    size_t newline = text.find('\n', i);
    if (newline == std::string::npos) {
      newline = text.size();
    }
    std::string line = Trim(text.substr(i, newline - i));
    i = newline + 1;
    if (first) {
      if (StartsWith(line, "\xEF\xBB\xBF")) {
        line = line.substr(3);
      }
      if (!StartsWith(line, "#EXTM3U")) {
        *error = "not an HLS playlist";
        return false;
      }
      first = false;
      continue;
    }
    if (line.empty()) {
      continue;
    }
    if (StartsWith(line, "#EXT-X-STREAM-INF:")) {
      auto attributes = ParseAttributes(line.substr(18));
      variant = HlsVariant();
      variant.bandwidth = ParseInteger(attributes["BANDWIDTH"]);
      variant.codecs = attributes["CODECS"];
      variant.audio = attributes["AUDIO"];
      variant.subtitles = attributes["SUBTITLES"];
      const std::string& resolution = attributes["RESOLUTION"];
      size_t x = resolution.find('x');
      if (x != std::string::npos) {
        variant.width = static_cast<int>(ParseInteger(resolution.substr(0, x)));
        variant.height =
            static_cast<int>(ParseInteger(resolution.substr(x + 1)));
      }
      pending_variant = true;
      playlist->master = true;
    } else if (StartsWith(line, "#EXT-X-MEDIA:")) {
      auto attributes = ParseAttributes(line.substr(13));
      HlsRendition rendition;
      const std::string& type = attributes["TYPE"];
      rendition.type = type == "AUDIO"       ? HlsRendition::Type::kAudio
                       : type == "VIDEO"     ? HlsRendition::Type::kVideo
                       : type == "SUBTITLES" ? HlsRendition::Type::kSubtitles
                                             : HlsRendition::Type::kOther;
      rendition.group = attributes["GROUP-ID"];
      rendition.name = attributes["NAME"];
      rendition.language = attributes["LANGUAGE"];
      rendition.is_default = attributes["DEFAULT"] == "YES";
      auto uri = attributes.find("URI");
      if (uri != attributes.end()) {
        rendition.url = ResolveUrl(base_url, uri->second);
      }
      playlist->renditions.push_back(std::move(rendition));
      playlist->master = true;
    } else if (StartsWith(line, "#EXTINF:")) {
      duration = ParseSeconds(line.substr(8));
    } else if (StartsWith(line, "#EXT-X-TARGETDURATION:")) {
      playlist->target_duration =
          std::min(ParseInteger(line.substr(22)), kMaxSeconds) *
          kMicrosPerSecond;
    } else if (StartsWith(line, "#EXT-X-MEDIA-SEQUENCE:")) {
      playlist->media_sequence = ParseInteger(line.substr(22));
    } else if (StartsWith(line, "#EXT-X-DISCONTINUITY-SEQUENCE:")) {
      discontinuity_base = ParseInteger(line.substr(30));
    } else if (line == "#EXT-X-DISCONTINUITY") {
      ++discontinuities;
    } else if (line == "#EXT-X-ENDLIST" || line == "#EXT-X-PLAYLIST-TYPE:VOD") {
      // A VOD playlist cannot change, ENDLIST or not.
      playlist->ended = true;
    } else if (StartsWith(line, "#EXT-X-BYTERANGE:")) {
      std::string range = line.substr(17);
      size_t at = range.find('@');
      range_length = ParseInteger(range);
      range_offset =
          at == std::string::npos ? -1 : ParseInteger(range.substr(at + 1));
    } else if (StartsWith(line, "#EXT-X-KEY:")) {
      key = ParseKey(line.substr(11), base_url);
    } else if (StartsWith(line, "#EXT-X-MAP:")) {
      auto attributes = ParseAttributes(line.substr(11));
      initialization = true;
      map = HlsMap();
      map.url = ResolveUrl(base_url, attributes["URI"]);
      map.key = key;
      auto range = attributes.find("BYTERANGE");
      if (range != attributes.end()) {
        const size_t at = range->second.find('@');
        map.length = ParseInteger(range->second);
        map.offset = at == std::string::npos
                         ? 0
                         : ParseInteger(range->second.substr(at + 1));
      }
    } else if (line[0] == '#') {
      continue;  // Comments and tags this player does not use.
    } else if (pending_variant) {
      variant.url = ResolveUrl(base_url, line);
      playlist->variants.push_back(variant);
      pending_variant = false;
    } else {
      if (playlist->segments.size() >= kMaxSegments) {
        *error = "HLS playlist with too many segments";
        return false;
      }
      HlsSegment segment;
      segment.url = ResolveUrl(base_url, line);
      segment.duration = std::max<int64_t>(duration, 0);
      segment.sequence = playlist->media_sequence +
                         static_cast<int64_t>(playlist->segments.size());
      segment.discontinuity = discontinuity_base + discontinuities;
      segment.key = key;
      segment.initialization = initialization;
      segment.map = map;
      if (range_length >= 0) {
        segment.length = range_length;
        segment.offset = range_offset;
        if (segment.offset < 0) {
          // Right after the previous range of the same resource.
          const HlsSegment* previous =
              playlist->segments.empty() ? nullptr : &playlist->segments.back();
          segment.offset =
              previous && previous->url == segment.url && previous->length >= 0
                  ? previous->offset + previous->length
                  : 0;
        }
      }
      playlist->segments.push_back(segment);
      duration = -1;
      range_length = range_offset = -1;
    }
  }
  if (first) {
    *error = "empty HLS playlist";
    return false;
  }
  return true;
}

bool HlsCodecsDecodable(const std::string& codecs) {
  size_t i = 0;
  while (i <= codecs.size()) {
    size_t comma = codecs.find(',', i);
    if (comma == std::string::npos) {
      comma = codecs.size();
    }
    std::string codec = Trim(codecs.substr(i, comma - i));
    if (!codec.empty() && !StartsWith(codec, "avc1") &&
        !StartsWith(codec, "avc3") && !StartsWith(codec, "hvc1") &&
        !StartsWith(codec, "hev1") && !StartsWith(codec, "mp4a")) {
      return false;
    }
    i = comma + 1;
  }
  return true;
}

int PickHlsVariant(const std::vector<HlsVariant>& variants) {
  int best = -1;
  for (size_t i = 0; i < variants.size(); ++i) {
    const HlsVariant& v = variants[i];
    if (!HlsCodecsDecodable(v.codecs)) {
      continue;
    }
    if (best < 0) {
      best = static_cast<int>(i);
      continue;
    }
    const HlsVariant& b = variants[best];
    if (v.bandwidth > b.bandwidth ||
        (v.bandwidth == b.bandwidth &&
         int64_t{v.width} * v.height > int64_t{b.width} * b.height)) {
      best = static_cast<int>(i);
    }
  }
  return best;
}

bool ParseHlsWebVtt(const std::string& text,
                    int64_t* mpegts,
                    std::vector<WebVttCue>* cues) {
  cues->clear();
  *mpegts = 0;
  std::vector<std::string> lines;
  size_t begin = 0;
  while (begin <= text.size()) {
    size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    begin = end + 1;
  }
  std::string& first = lines[0];
  if (StartsWith(first, "\xEF\xBB\xBF")) {
    first = first.substr(3);
  }
  if (!StartsWith(first, "WEBVTT") ||
      (first.size() > 6 && first[6] != ' ' && first[6] != '\t')) {
    return false;
  }
  int64_t local = 0;
  size_t i = 1;
  // The header, up to the first blank line.
  for (; i < lines.size() && !lines[i].empty(); ++i) {
    if (!StartsWith(lines[i], "X-TIMESTAMP-MAP=")) {
      continue;
    }
    const std::string& map = lines[i];
    size_t at = map.find("LOCAL:");
    if (at != std::string::npos) {
      at += 6;
      const int64_t time = ParseVttTime(map, &at);
      if (time >= 0) {
        local = time;
      }
    }
    at = map.find("MPEGTS:");
    if (at != std::string::npos) {
      *mpegts = ParseInteger(map.substr(at + 7)) & ((int64_t{1} << 33) - 1);
    }
  }
  // Blocks: a cue is an optional identifier, a timing line and its
  // payload; NOTE, STYLE and REGION blocks have no timing line.
  while (i < lines.size()) {
    if (lines[i].empty()) {
      ++i;
      continue;
    }
    size_t timing = i;
    if (lines[i].find("-->") == std::string::npos) {
      timing = i + 1;
    }
    if (timing < lines.size() &&
        lines[timing].find("-->") != std::string::npos) {
      const std::string& line = lines[timing];
      size_t pos = 0;
      while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
      }
      const int64_t start = ParseVttTime(line, &pos);
      size_t arrow = line.find("-->", pos);
      int64_t end = -1;
      if (start >= 0 && arrow != std::string::npos) {
        pos = arrow + 3;
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
          ++pos;
        }
        end = ParseVttTime(line, &pos);
      }
      WebVttCue cue;
      for (i = timing + 1; i < lines.size() && !lines[i].empty(); ++i) {
        if (!cue.text.empty()) {
          cue.text += '\n';
        }
        cue.text += lines[i];
      }
      if (start >= 0 && end > start && cues->size() < kMaxSegments) {
        cue.start = start - local;
        cue.end = end - local;
        cues->push_back(std::move(cue));
      }
      continue;
    }
    while (i < lines.size() && !lines[i].empty()) {
      ++i;
    }
  }
  return true;
}

}  // namespace lychee
