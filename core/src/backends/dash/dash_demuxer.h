#ifndef LYCHEE_BACKENDS_DASH_DASH_DEMUXER_H_
#define LYCHEE_BACKENDS_DASH_DASH_DEMUXER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "backends/dash/mpd.h"
#include "backends/hls/hls_demuxer.h"
#include "media/data_source.h"

namespace lychee {

// MPEG-DASH (ISO/IEC 23009-1), static (VOD) manifests, for every build:
// FFmpeg's dashdec needs libxml2, which the shipped FFmpeg does not
// enable. The manifest's first Period becomes what HlsDemuxer plays: each
// video Representation (H.264 or HEVC, fragmented MP4 or MPEG-TS) a
// variant, adapting between those of one codec as HLS does
// (SetVariantPolicy(), the bandwidth estimate); each audio AdaptationSet
// an audio rendition (its highest-bandwidth Representation, with lang,
// Label and Role); each text AdaptationSet a subtitle rendition (a WebVTT
// sidecar file, or fMP4 IMSC/WebVTT segments). Segments come from
// SegmentTemplate ($Number$, $Time$, $RepresentationID$, $Bandwidth$;
// SegmentTimeline or @duration), SegmentList, or SegmentBase with a
// 'sidx' index, fetched once its Representation first plays.
class DashDemuxer : public HlsDemuxer {
 public:
  DashDemuxer();
  ~DashDemuxer() override;

  // Whether |data| (the input's first bytes) looks like an MPD.
  static bool Detect(const uint8_t* data, size_t size);

  bool Open(const std::string& url, std::string* error) override;
  // Takes the manifest's source, already opened by the format probe.
  bool Open(std::unique_ptr<DataSource> source,
            const std::string& url,
            std::string* error) override;
  void Interrupt() override;
  void Abort() override;

 private:
  // The playlist set_playlist_source() asks for: the master playlist at
  // the manifest's URL, else a Representation's.
  bool Playlist(const std::string& url,
                HlsPlaylist* playlist,
                std::string* error);
  // Reads |length| bytes (-1: all, up to a limit) of |url| from |offset|.
  bool Fetch(const std::string& url,
             int64_t offset,
             int64_t length,
             std::string* data,
             std::string* error);

  DashManifest manifest_;
  // The manifest's URL after redirects, the master playlist made of it,
  // and the Representations its variants and renditions name
  // (adaptation set, representation).
  std::string url_;
  HlsPlaylist master_;
  struct Named {
    std::string url;
    size_t set = 0;
    size_t representation = 0;
  };
  std::vector<Named> named_;

  // The fetch in progress, for Interrupt() and Abort().
  std::mutex fetch_mutex_;
  DataSource* fetching_ = nullptr;
  std::atomic<bool> aborted_{false};
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_DASH_DASH_DEMUXER_H_
