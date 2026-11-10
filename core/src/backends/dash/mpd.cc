#include "backends/dash/mpd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "backends/dash/xml.h"

namespace lychee {

namespace {

// More would be a manifest built to exhaust memory.
constexpr size_t kMaxSegments = 200000;

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                        s.front() == '\r' || s.front() == '\n')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                        s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
}

bool ParseInt(std::string_view text, int64_t* value) {
  text = Trim(text);
  if (text.empty()) {
    return false;
  }
  int64_t result = 0;
  for (char c : text) {
    if (c < '0' || c > '9' ||
        result > (std::numeric_limits<int64_t>::max() - (c - '0')) / 10) {
      return false;
    }
    result = result * 10 + (c - '0');
  }
  *value = result;
  return true;
}

// The attribute as a non-negative integer, or |fallback|.
int64_t IntAttribute(const XmlElement& element,
                     std::string_view name,
                     int64_t fallback) {
  int64_t value;
  const std::string* text = element.Attribute(name);
  return text && ParseInt(*text, &value) ? value : fallback;
}

std::string StringAttribute(const XmlElement& element, std::string_view name) {
  const std::string* text = element.Attribute(name);
  return text ? std::string(Trim(*text)) : std::string();
}

// "first-last", inclusive.
bool ParseRange(std::string_view text, int64_t* offset, int64_t* length) {
  const size_t dash = text.find('-');
  int64_t first;
  int64_t last;
  if (dash == std::string_view::npos ||
      !ParseInt(text.substr(0, dash), &first) ||
      !ParseInt(text.substr(dash + 1), &last) || last < first) {
    return false;
  }
  *offset = first;
  *length = last - first + 1;
  return true;
}

// "30", "30000/1001", "29.97".
double ParseFrameRate(std::string_view text) {
  text = Trim(text);
  const size_t slash = text.find('/');
  int64_t num;
  int64_t den;
  if (slash != std::string_view::npos) {
    if (ParseInt(text.substr(0, slash), &num) &&
        ParseInt(text.substr(slash + 1), &den) && den > 0) {
      return static_cast<double>(num) / static_cast<double>(den);
    }
    return 0;
  }
  const std::string copy(text);
  char* end = nullptr;
  const double rate = strtod(copy.c_str(), &end);
  return end && *end == '\0' && rate > 0 && rate < 1000 ? rate : 0;
}

// |time| units of |timescale| in microseconds.
int64_t ToMicros(int64_t time, int64_t timescale) {
  if (timescale <= 0) {
    return 0;
  }
  const long double micros = static_cast<long double>(time) * 1e6L /
                             static_cast<long double>(timescale);
  if (micros >= static_cast<long double>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }
  return static_cast<int64_t>(std::llround(micros));
}

// The BaseURL child resolved against |base|, or |base|.
std::string Resolve(const XmlElement& element, const std::string& base) {
  const XmlElement* base_url = element.Child("BaseURL");
  if (!base_url) {
    return base;
  }
  const std::string_view text = Trim(base_url->text);
  return text.empty() ? base : ResolveUrl(base, std::string(text));
}

// The segment information of one level merged into what the levels above
// set: SegmentBase, SegmentList and SegmentTemplate apart, each taking the
// attributes this level gives. |chosen| becomes the kind this level has,
// a template before a list before a base.
void MergeSegments(const XmlElement& level,
                   const std::string& base,
                   DashSegments inherited[3],
                   DashSegments::Kind* chosen) {
  static constexpr std::pair<const char*, DashSegments::Kind> kKinds[] = {
      {"SegmentBase", DashSegments::Kind::kBase},
      {"SegmentList", DashSegments::Kind::kList},
      {"SegmentTemplate", DashSegments::Kind::kTemplate},
  };
  for (int k = 0; k < 3; ++k) {
    const XmlElement* element = level.Child(kKinds[k].first);
    if (!element) {
      continue;
    }
    DashSegments& s = inherited[k];
    s.kind = kKinds[k].second;
    *chosen = s.kind;
    s.timescale =
        std::max<int64_t>(1, IntAttribute(*element, "timescale", s.timescale));
    s.duration = IntAttribute(*element, "duration", s.duration);
    s.start_number = IntAttribute(*element, "startNumber", s.start_number);
    if (const std::string* range = element->Attribute("indexRange")) {
      if (!ParseRange(*range, &s.index_offset, &s.index_length)) {
        s.index_offset = s.index_length = -1;
      }
    }
    if (const std::string* media = element->Attribute("media")) {
      s.media = std::string(Trim(*media));
    }
    if (const std::string* init = element->Attribute("initialization")) {
      s.initialization = std::string(Trim(*init));
    }
    if (const XmlElement* init = element->Child("Initialization")) {
      const std::string source = StringAttribute(*init, "sourceURL");
      if (s.kind == DashSegments::Kind::kTemplate) {
        if (!source.empty()) {
          s.initialization = source;
        }
      } else {
        s.initialization_url = source.empty() ? "" : ResolveUrl(base, source);
      }
      if (const std::string* range = init->Attribute("range")) {
        if (!ParseRange(*range, &s.initialization_offset,
                        &s.initialization_length)) {
          s.initialization_offset = s.initialization_length = -1;
        }
      }
    }
    const XmlElement* timeline = element->Child("SegmentTimeline");
    if (!timeline && element->Attribute("duration")) {
      // Segments of a fixed duration here, not the timeline above.
      s.timeline.clear();
    }
    if (timeline) {
      s.timeline.clear();
      for (const XmlElement* entry : timeline->Children("S")) {
        DashSegments::Entry e;
        e.t = IntAttribute(*entry, "t", -1);
        e.d = IntAttribute(*entry, "d", 0);
        const std::string r = StringAttribute(*entry, "r");
        int64_t repeat = 0;
        if (r == "-1") {
          e.r = -1;
        } else if (ParseInt(r, &repeat)) {
          e.r = repeat;
        }
        if (e.d > 0) {
          s.timeline.push_back(e);
        }
      }
    }
    if (s.kind == DashSegments::Kind::kList) {
      const std::vector<const XmlElement*> urls =
          element->Children("SegmentURL");
      if (!urls.empty()) {
        s.urls.clear();
        for (const XmlElement* url : urls) {
          DashSegments::Url u;
          const std::string media = StringAttribute(*url, "media");
          if (!media.empty()) {
            u.media = ResolveUrl(base, media);
          }
          if (const std::string* range = url->Attribute("mediaRange")) {
            if (!ParseRange(*range, &u.offset, &u.length)) {
              u.offset = u.length = -1;
            }
          }
          s.urls.push_back(std::move(u));
        }
      }
    }
  }
}

// What a Role or Accessibility descriptor says.
void ReadRole(const XmlElement& descriptor, DashAdaptationSet* set) {
  const std::string scheme = StringAttribute(descriptor, "schemeIdUri");
  const std::string value = StringAttribute(descriptor, "value");
  if (scheme == "urn:mpeg:dash:role:2011") {
    if (value == "main") {
      set->main = true;
    } else if (value == "commentary") {
      set->role = TrackRole::kCommentary;
    } else if (value == "description") {
      set->role = TrackRole::kDescription;
    } else if (value == "caption" ||
               value == "enhanced-audio-intelligibility") {
      set->role = TrackRole::kHearingImpaired;
    } else if (value == "forced-subtitle") {
      set->forced = true;
    }
  } else if (scheme == "urn:tva:metadata:cs:AudioPurposeCS:2007") {
    if (value == "1") {
      set->role = TrackRole::kDescription;
    } else if (value == "2") {
      set->role = TrackRole::kHearingImpaired;
    }
  }
}

DashAdaptationSet::Type TypeOf(std::string_view content_type,
                               std::string_view mime_type,
                               std::string_view codecs) {
  auto starts = [](std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
  };
  if (content_type == "video" || starts(mime_type, "video/")) {
    return DashAdaptationSet::Type::kVideo;
  }
  if (content_type == "audio" || starts(mime_type, "audio/")) {
    return DashAdaptationSet::Type::kAudio;
  }
  if (content_type == "text" || starts(mime_type, "text/") ||
      starts(codecs, "stpp") || starts(codecs, "wvtt")) {
    return DashAdaptationSet::Type::kText;
  }
  return DashAdaptationSet::Type::kOther;
}

}  // namespace

bool ParseIsoDuration(std::string_view text, int64_t* micros) {
  text = Trim(text);
  if (text.empty() || text[0] != 'P') {
    return false;
  }
  long double total = 0;
  bool in_time = false;
  bool any = false;
  size_t i = 1;
  while (i < text.size()) {
    if (text[i] == 'T') {
      in_time = true;
      ++i;
      continue;
    }
    const size_t start = i;
    while (i < text.size() &&
           ((text[i] >= '0' && text[i] <= '9') || text[i] == '.')) {
      ++i;
    }
    if (i == start || i >= text.size()) {
      return false;
    }
    const std::string number(text.substr(start, i - start));
    char* end = nullptr;
    const long double value = strtold(number.c_str(), &end);
    if (!end || *end != '\0') {
      return false;
    }
    long double unit;
    switch (text[i]) {
      case 'Y':
        unit = 365.25L * 86400;
        break;
      case 'M':
        unit = in_time ? 60 : 30.4375L * 86400;
        break;
      case 'W':
        unit = 7 * 86400;
        break;
      case 'D':
        unit = 86400;
        break;
      case 'H':
        unit = 3600;
        break;
      case 'S':
        unit = 1;
        break;
      default:
        return false;
    }
    total += value * unit;
    any = true;
    ++i;
  }
  // A century at most.
  if (!any || total < 0 || total > 3.2e9L) {
    return false;
  }
  *micros = static_cast<int64_t>(std::llround(total * 1e6L));
  return true;
}

std::string ExpandDashTemplate(std::string_view pattern,
                               const DashRepresentation& representation,
                               int64_t number,
                               int64_t time) {
  std::string out;
  size_t i = 0;
  while (i < pattern.size()) {
    const size_t open = pattern.find('$', i);
    if (open == std::string_view::npos) {
      out.append(pattern.substr(i));
      break;
    }
    out.append(pattern.substr(i, open - i));
    const size_t close = pattern.find('$', open + 1);
    if (close == std::string_view::npos) {
      out.append(pattern.substr(open));
      break;
    }
    std::string_view identifier = pattern.substr(open + 1, close - open - 1);
    i = close + 1;
    if (identifier.empty()) {
      out.push_back('$');
      continue;
    }
    // A width: %0Nd (or %Nd, %d).
    int width = 0;
    const size_t percent = identifier.find('%');
    bool valid = true;
    if (percent != std::string_view::npos) {
      std::string_view format = identifier.substr(percent + 1);
      identifier = identifier.substr(0, percent);
      if (format.empty() || format.back() != 'd') {
        valid = false;
      } else {
        format.remove_suffix(1);
        if (!format.empty() && format.front() == '0') {
          format.remove_prefix(1);
        }
        int64_t parsed = 0;
        if (!format.empty() && (!ParseInt(format, &parsed) || parsed > 32)) {
          valid = false;
        }
        width = static_cast<int>(parsed);
      }
    }
    int64_t value;
    if (valid && identifier == "RepresentationID" &&
        percent == std::string_view::npos) {
      out += representation.id;
      continue;
    }
    if (valid && identifier == "Number") {
      value = number;
    } else if (valid && identifier == "Bandwidth") {
      value = representation.bandwidth;
    } else if (valid && identifier == "Time") {
      value = time;
    } else {
      // Not an identifier: kept as it is.
      out.append(pattern.substr(open, close - open + 1));
      continue;
    }
    char digits[64];
    snprintf(digits, sizeof(digits), "%0*lld", width,
             static_cast<long long>(value));
    out += digits;
  }
  return out;
}

bool ParseMpd(std::string_view text,
              const std::string& url,
              DashManifest* manifest,
              std::string* error) {
  *manifest = {};
  XmlElement root;
  if (!ParseXml(text, &root, error)) {
    return false;
  }
  if (root.name != "MPD") {
    *error = "not an MPD: the root element is <" + root.name + ">";
    return false;
  }
  manifest->dynamic = StringAttribute(root, "type") == "dynamic";
  int64_t duration;
  if (ParseIsoDuration(StringAttribute(root, "mediaPresentationDuration"),
                       &duration)) {
    manifest->duration = duration;
  }
  const std::vector<const XmlElement*> periods = root.Children("Period");
  manifest->periods = periods.size();
  if (periods.empty()) {
    *error = "MPD without a Period";
    return false;
  }
  const XmlElement& period = *periods[0];
  if (ParseIsoDuration(StringAttribute(period, "duration"), &duration)) {
    manifest->duration = duration;
  }
  const std::string period_base = Resolve(period, Resolve(root, url));
  DashSegments period_segments[3];
  DashSegments::Kind period_kind = DashSegments::Kind::kNone;
  MergeSegments(period, period_base, period_segments, &period_kind);

  for (const XmlElement* set_element : period.Children("AdaptationSet")) {
    DashAdaptationSet set;
    const std::string set_base = Resolve(*set_element, period_base);
    set.language = StringAttribute(*set_element, "lang");
    if (const XmlElement* label = set_element->Child("Label")) {
      set.label = std::string(Trim(label->text));
    } else {
      set.label = StringAttribute(*set_element, "label");
    }
    for (const XmlElement* role : set_element->Children("Role")) {
      ReadRole(*role, &set);
    }
    for (const XmlElement* role : set_element->Children("Accessibility")) {
      ReadRole(*role, &set);
    }
    DashSegments set_segments[3] = {period_segments[0], period_segments[1],
                                    period_segments[2]};
    DashSegments::Kind set_kind = period_kind;
    MergeSegments(*set_element, set_base, set_segments, &set_kind);
    const std::string set_mime = StringAttribute(*set_element, "mimeType");
    const std::string set_codecs = StringAttribute(*set_element, "codecs");
    const std::string content_type =
        StringAttribute(*set_element, "contentType");
    const int set_width =
        static_cast<int>(IntAttribute(*set_element, "width", 0));
    const int set_height =
        static_cast<int>(IntAttribute(*set_element, "height", 0));
    const double set_rate =
        ParseFrameRate(StringAttribute(*set_element, "frameRate"));
    for (const XmlElement* rep : set_element->Children("Representation")) {
      DashRepresentation r;
      r.id = StringAttribute(*rep, "id");
      r.bandwidth = IntAttribute(*rep, "bandwidth", 0);
      r.width = static_cast<int>(
          std::min<int64_t>(IntAttribute(*rep, "width", set_width), 1 << 16));
      r.height = static_cast<int>(
          std::min<int64_t>(IntAttribute(*rep, "height", set_height), 1 << 16));
      const double rate = ParseFrameRate(StringAttribute(*rep, "frameRate"));
      r.frame_rate = rate > 0 ? rate : set_rate;
      r.codecs = StringAttribute(*rep, "codecs");
      if (r.codecs.empty()) {
        r.codecs = set_codecs;
      }
      r.mime_type = StringAttribute(*rep, "mimeType");
      if (r.mime_type.empty()) {
        r.mime_type = set_mime;
      }
      r.base_url = Resolve(*rep, set_base);
      DashSegments segments[3] = {set_segments[0], set_segments[1],
                                  set_segments[2]};
      DashSegments::Kind kind = set_kind;
      MergeSegments(*rep, r.base_url, segments, &kind);
      for (const DashSegments& s : segments) {
        if (s.kind == kind && kind != DashSegments::Kind::kNone) {
          r.segments = s;
        }
      }
      if (set.type == DashAdaptationSet::Type::kOther) {
        set.type = TypeOf(content_type, r.mime_type, r.codecs);
      }
      set.representations.push_back(std::move(r));
    }
    if (set.type == DashAdaptationSet::Type::kOther) {
      set.type = TypeOf(content_type, set_mime, set_codecs);
    }
    if (!set.representations.empty()) {
      manifest->adaptation_sets.push_back(std::move(set));
    }
  }
  if (manifest->adaptation_sets.empty()) {
    *error = "MPD without Representations";
    return false;
  }
  return true;
}

bool ParseSidx(const uint8_t* data,
               size_t size,
               int64_t offset,
               std::vector<DashSubsegment>* subsegments) {
  auto u32 = [data](size_t at) {
    return (uint32_t{data[at]} << 24) | (uint32_t{data[at + 1]} << 16) |
           (uint32_t{data[at + 2]} << 8) | uint32_t{data[at + 3]};
  };
  auto u64 = [&u32](size_t at) {
    return (uint64_t{u32(at)} << 32) | u32(at + 4);
  };
  subsegments->clear();
  if (size < 8 || data[4] != 's' || data[5] != 'i' || data[6] != 'd' ||
      data[7] != 'x') {
    return false;
  }
  uint64_t box_size = u32(0);
  size_t at = 8;
  if (box_size == 1) {
    if (size < 16) {
      return false;
    }
    box_size = u64(8);
    at = 16;
  }
  if (size < at + 12) {
    return false;
  }
  const uint8_t version = data[at];
  at += 4;  // Version and flags.
  at += 4;  // reference_ID.
  const uint32_t timescale = u32(at);
  at += 4;
  uint64_t first_offset;
  if (version == 0) {
    if (size < at + 8) return false;
    first_offset = u32(at + 4);
    at += 8;
  } else {
    if (size < at + 16) return false;
    first_offset = u64(at + 8);
    at += 16;
  }
  if (size < at + 4 || timescale == 0 || box_size < at ||
      box_size > (uint64_t{1} << 40) || first_offset > (uint64_t{1} << 50)) {
    return false;
  }
  const uint16_t count =
      static_cast<uint16_t>((data[at + 2] << 8) | data[at + 3]);
  at += 4;
  if (size < at + size_t{count} * 12) {
    return false;
  }
  int64_t position = offset + static_cast<int64_t>(box_size) +
                     static_cast<int64_t>(first_offset);
  for (uint16_t i = 0; i < count; ++i, at += 12) {
    const uint32_t reference = u32(at);
    if (reference & 0x80000000u) {
      // Another sidx: hierarchical indexes are not read.
      subsegments->clear();
      return false;
    }
    DashSubsegment subsegment;
    subsegment.offset = position;
    subsegment.length = reference & 0x7fffffffu;
    subsegment.duration = ToMicros(u32(at + 4), timescale);
    position += subsegment.length;
    subsegments->push_back(subsegment);
  }
  return !subsegments->empty();
}

bool DashMediaPlaylist(const DashRepresentation& representation,
                       DashAdaptationSet::Type type,
                       int64_t duration,
                       const std::vector<DashSubsegment>& index,
                       HlsPlaylist* playlist,
                       std::string* error) {
  *playlist = {};
  playlist->ended = true;
  const DashSegments& s = representation.segments;
  const bool ts = representation.mime_type == "video/mp2t";
  const bool vtt = representation.mime_type == "text/vtt";
  if (type == DashAdaptationSet::Type::kText && vtt) {
    // A sidecar file: one segment, its cues on the Period's timeline.
    HlsSegment segment;
    segment.url = representation.base_url;
    segment.duration = duration != kNoTimestamp ? duration : 0;
    playlist->segments.push_back(segment);
    playlist->target_duration = segment.duration;
    return true;
  }
  // Segments of fragmented MP4 start after their initialization; TS and
  // self-initializing ones carry it.
  HlsMap map;
  bool initialization = false;
  auto add = [&](const std::string& url, int64_t offset, int64_t length,
                 int64_t micros, int64_t number) {
    HlsSegment segment;
    segment.url = url;
    segment.offset = offset;
    segment.length = length;
    segment.duration = std::max<int64_t>(micros, 0);
    segment.sequence = number;
    segment.initialization = initialization;
    segment.map = map;
    playlist->target_duration =
        std::max(playlist->target_duration, segment.duration);
    playlist->segments.push_back(std::move(segment));
    return playlist->segments.size() < kMaxSegments;
  };
  // The end of the Period in |s.timescale| units, or -1.
  int64_t end = -1;
  if (duration != kNoTimestamp) {
    const long double units = static_cast<long double>(duration) *
                              static_cast<long double>(s.timescale) / 1e6L;
    end = units >= 9e18L ? int64_t{9000000000000000000}
                         : static_cast<int64_t>(units);
  }
  switch (s.kind) {
    case DashSegments::Kind::kTemplate: {
      if (s.media.empty()) {
        *error = "SegmentTemplate without media";
        return false;
      }
      if (!s.initialization.empty()) {
        map.url = ResolveUrl(
            representation.base_url,
            ExpandDashTemplate(s.initialization, representation, 0, 0));
        initialization = true;
      }
      int64_t number = s.start_number;
      auto url = [&](int64_t time) {
        return ResolveUrl(
            representation.base_url,
            ExpandDashTemplate(s.media, representation, number, time));
      };
      if (!s.timeline.empty()) {
        // How often entry |i|, starting at |time|, repeats.
        auto repeats_of = [&](size_t i, int64_t time) {
          const DashSegments::Entry& e = s.timeline[i];
          if (e.r >= 0) {
            return e.r;
          }
          // Up to the next entry's time, or the end of the Period.
          const int64_t until =
              i + 1 < s.timeline.size() && s.timeline[i + 1].t >= 0
                  ? s.timeline[i + 1].t
                  : end;
          return until > time ? (until - time + e.d - 1) / e.d - 1 : int64_t{0};
        };
        // Count the segments before expanding any: each costs a template
        // expansion and a URL resolution, and a short d with r="-1" asks
        // for hundreds of thousands (as the @duration case below does).
        int64_t count = 0;
        int64_t time = 0;
        for (size_t i = 0; i < s.timeline.size(); ++i) {
          const DashSegments::Entry& e = s.timeline[i];
          if (e.t >= 0) {
            time = e.t;
          }
          const int64_t repeats = repeats_of(i, time);
          // add() refuses the kMaxSegments-th.
          if (repeats >= static_cast<int64_t>(kMaxSegments) - 1 - count) {
            *error = "too many segments";
            return false;
          }
          count += repeats + 1;
          if (repeats + 1 >
              (std::numeric_limits<int64_t>::max() - time) / e.d) {
            // The expansion below stops there with an error.
            break;
          }
          time += (repeats + 1) * e.d;
        }
        time = 0;
        for (size_t i = 0; i < s.timeline.size(); ++i) {
          const DashSegments::Entry& e = s.timeline[i];
          if (e.t >= 0) {
            time = e.t;
          }
          const int64_t repeats = repeats_of(i, time);
          for (int64_t k = 0; k <= repeats; ++k) {
            if (!add(url(time), -1, -1, ToMicros(e.d, s.timescale), number)) {
              *error = "too many segments";
              return false;
            }
            ++number;
            if (time > std::numeric_limits<int64_t>::max() - e.d) {
              *error = "segment times overflow";
              return false;
            }
            time += e.d;
          }
        }
      } else if (s.duration > 0) {
        if (end < 0) {
          *error = "SegmentTemplate@duration without a presentation duration";
          return false;
        }
        const int64_t count = std::max<int64_t>(
            1, end / s.duration + (end % s.duration != 0 ? 1 : 0));
        if (count > static_cast<int64_t>(kMaxSegments)) {
          *error = "too many segments";
          return false;
        }
        for (int64_t k = 0; k < count; ++k) {
          const int64_t time = k * s.duration;
          // The last one ends with the Period.
          const int64_t length = std::min(s.duration, end - time);
          // url() reads |number|: the increment is not in the arguments,
          // whose order of evaluation is unspecified (GCC and MSVC took
          // the next segment's number).
          add(url(time), -1, -1, ToMicros(length, s.timescale), number);
          ++number;
        }
      } else {
        *error = "SegmentTemplate without a duration or a SegmentTimeline";
        return false;
      }
      break;
    }
    case DashSegments::Kind::kList: {
      if (s.initialization_offset >= 0 || !s.initialization_url.empty()) {
        map.url = s.initialization_url.empty() ? representation.base_url
                                               : s.initialization_url;
        map.offset = s.initialization_offset;
        map.length = s.initialization_length;
        initialization = true;
      }
      int64_t number = s.start_number;
      size_t entry = 0;
      int64_t repeated = 0;
      for (const DashSegments::Url& u : s.urls) {
        int64_t length = s.duration;
        if (entry < s.timeline.size()) {
          length = s.timeline[entry].d;
          if (++repeated > s.timeline[entry].r) {
            ++entry;
            repeated = 0;
          }
        }
        if (!add(u.media.empty() ? representation.base_url : u.media, u.offset,
                 u.length, ToMicros(length, s.timescale), number++)) {
          *error = "too many segments";
          return false;
        }
      }
      if (s.urls.empty()) {
        *error = "SegmentList without SegmentURLs";
        return false;
      }
      break;
    }
    case DashSegments::Kind::kBase: {
      map.url = s.initialization_url.empty() ? representation.base_url
                                             : s.initialization_url;
      if (s.initialization_offset >= 0) {
        map.offset = s.initialization_offset;
        map.length = s.initialization_length;
      } else if (s.index_offset > 0) {
        // What comes before the index: 'ftyp' and 'moov'.
        map.offset = 0;
        map.length = s.index_offset;
      }
      initialization = map.length > 0;
      if (index.empty()) {
        *error = "SegmentBase without a segment index";
        return false;
      }
      int64_t number = s.start_number;
      for (const DashSubsegment& subsegment : index) {
        add(representation.base_url, subsegment.offset, subsegment.length,
            subsegment.duration, number++);
      }
      break;
    }
    case DashSegments::Kind::kNone:
      if (!ts) {
        *error = "a Representation without segment information";
        return false;
      }
      add(representation.base_url, -1, -1,
          duration != kNoTimestamp ? duration : 0, 1);
      break;
  }
  if (!initialization && !ts) {
    *error = "fragmented MP4 without an initialization segment";
    return false;
  }
  playlist->media_sequence = playlist->segments.front().sequence;
  return true;
}

}  // namespace lychee
