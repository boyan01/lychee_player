#ifndef LYCHEE_BACKENDS_DASH_MPD_H_
#define LYCHEE_BACKENDS_DASH_MPD_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "backends/hls/hls_playlist.h"
#include "base/time.h"
#include "media/stream_info.h"

namespace lychee {

// How a Representation's segments are addressed (ISO/IEC 23009-1 5.3.9),
// with what the Period and AdaptationSet levels set inherited.
struct DashSegments {
  enum class Kind { kNone, kBase, kList, kTemplate };
  Kind kind = Kind::kNone;
  int64_t timescale = 1;
  // Of each segment, in |timescale| units; 0 with a timeline.
  int64_t duration = 0;
  int64_t start_number = 1;
  // SegmentTemplate@media and @initialization, unexpanded.
  std::string media;
  std::string initialization;
  // Initialization@sourceURL (SegmentBase, SegmentList), resolved, or
  // empty for the Representation's BaseURL; and @range, or -1.
  std::string initialization_url;
  int64_t initialization_offset = -1;
  int64_t initialization_length = -1;
  // SegmentBase@indexRange: where the 'sidx' box is, or -1.
  int64_t index_offset = -1;
  int64_t index_length = -1;
  // SegmentTimeline's S elements: t (-1 if absent), d, r (-1: up to the
  // end of the Period).
  struct Entry {
    int64_t t = -1;
    int64_t d = 0;
    int64_t r = 0;
  };
  std::vector<Entry> timeline;
  // SegmentList's SegmentURLs: media resolved (empty: the BaseURL), and
  // mediaRange, or -1.
  struct Url {
    std::string media;
    int64_t offset = -1;
    int64_t length = -1;
  };
  std::vector<Url> urls;
};

struct DashRepresentation {
  std::string id;
  int64_t bandwidth = 0;
  int width = 0;
  int height = 0;
  double frame_rate = 0;
  std::string codecs;
  std::string mime_type;
  // Resolved through every BaseURL level.
  std::string base_url;
  DashSegments segments;
};

struct DashAdaptationSet {
  enum class Type { kVideo, kAudio, kText, kOther };
  Type type = Type::kOther;
  std::string language;
  // The Label element.
  std::string label;
  // Role "main"; Role "forced-subtitle".
  bool main = false;
  bool forced = false;
  TrackRole role = TrackRole::kMain;
  std::vector<DashRepresentation> representations;
};

struct DashManifest {
  // type="dynamic": live.
  bool dynamic = false;
  // Of the first Period, microseconds, or kNoTimestamp if not given.
  int64_t duration = kNoTimestamp;
  size_t periods = 0;
  std::vector<DashAdaptationSet> adaptation_sets;
};

// Parses an MPD's first Period, resolving URLs against |url| (where it
// came from, after redirects). False with |error| set if it is not an MPD.
bool ParseMpd(std::string_view text,
              const std::string& url,
              DashManifest* manifest,
              std::string* error);

// An ISO 8601 duration ("PT1H2M3.5S", "P1DT2H"), in microseconds; false
// if malformed.
bool ParseIsoDuration(std::string_view text, int64_t* micros);

// A SegmentTemplate string with $RepresentationID$, $Number$,
// $Bandwidth$ and $Time$ (each with an optional %0Nd width) and $$
// replaced.
std::string ExpandDashTemplate(std::string_view pattern,
                               const DashRepresentation& representation,
                               int64_t number,
                               int64_t time);

// A segment index: the subsegments one 'sidx' box lists, in byte ranges
// of the resource and microseconds.
struct DashSubsegment {
  int64_t offset = 0;
  int64_t length = 0;
  int64_t duration = 0;
};

// Parses the 'sidx' box at the start of |data|, which was read from
// |offset| in the resource. False if it is not one, or it refers to other
// index boxes (hierarchical indexes).
bool ParseSidx(const uint8_t* data,
               size_t size,
               int64_t offset,
               std::vector<DashSubsegment>* subsegments);

// |representation|'s segments as an HLS media playlist (VOD), each with
// its initialization as the map, for a Period of |duration| microseconds
// (kNoTimestamp if unknown). A SegmentBase takes |index|, its
// subsegments (ParseSidx). WebVTT is one segment, the whole file. False
// with |error| set if they cannot be listed.
bool DashMediaPlaylist(const DashRepresentation& representation,
                       DashAdaptationSet::Type type,
                       int64_t duration,
                       const std::vector<DashSubsegment>& index,
                       HlsPlaylist* playlist,
                       std::string* error);

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_DASH_MPD_H_
