#ifndef LYCHEE_BACKENDS_HLS_HLS_PLAYLIST_H_
#define LYCHEE_BACKENDS_HLS_HLS_PLAYLIST_H_

#include <cstdint>
#include <string>
#include <vector>

namespace lychee {

// A variant stream of a master playlist (RFC 8216 4.3.4.2).
struct HlsVariant {
  std::string url;
  int64_t bandwidth = 0;
  int width = 0;
  int height = 0;
  std::string codecs;
};

// A media segment (RFC 8216 3), its URL resolved.
struct HlsSegment {
  std::string url;
  // Microseconds, from EXTINF.
  int64_t duration = 0;
  int64_t sequence = 0;
  // EXT-X-DISCONTINUITY-SEQUENCE plus the discontinuities before it:
  // segments with the same number share one timeline.
  int64_t discontinuity = 0;
  // EXT-X-BYTERANGE, or -1 for the whole resource.
  int64_t offset = -1;
  int64_t length = -1;
  // EXT-X-KEY with a METHOD other than NONE applies.
  bool encrypted = false;
  // EXT-X-MAP applies: fragmented MP4 segments.
  bool initialization = false;
};

struct HlsPlaylist {
  // A master playlist has variants and no segments.
  bool master = false;
  std::vector<HlsVariant> variants;

  int64_t target_duration = 0;  // Microseconds.
  int64_t media_sequence = 0;
  bool ended = false;  // EXT-X-ENDLIST, or PLAYLIST-TYPE VOD.
  std::vector<HlsSegment> segments;
};

// Parses |text|, resolving URLs against |base_url| (the playlist's URL
// after redirects). False with |error| set if it is not a playlist.
bool ParseHlsPlaylist(const std::string& text,
                      const std::string& base_url,
                      HlsPlaylist* playlist,
                      std::string* error);

// |reference| resolved against |base| (RFC 3986 5.2), for URLs and for
// plain file paths.
std::string ResolveUrl(const std::string& base, const std::string& reference);

// The variant to play: the highest bandwidth among those whose CODECS
// this build decodes (H.264, HEVC, AAC; any without CODECS), the larger
// picture on a tie. -1 if there are none.
int PickHlsVariant(const std::vector<HlsVariant>& variants);

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_HLS_HLS_PLAYLIST_H_
