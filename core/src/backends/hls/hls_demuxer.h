#ifndef LYCHEE_BACKENDS_HLS_HLS_DEMUXER_H_
#define LYCHEE_BACKENDS_HLS_HLS_DEMUXER_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "backends/hls/hls_playlist.h"
#include "backends/mp2t/ts_reader.h"
#include "backends/mp4/mp4_demuxer.h"
#include "media/annexb.h"
#include "media/data_source.h"
#include "media/demuxer.h"

namespace lychee {

// HTTP Live Streaming (RFC 8216) with MPEG-TS or fragmented MP4
// (EXT-X-MAP) segments, for builds without FFmpeg. A master playlist
// plays one variant (PickHlsVariant) and lists the others it can decode
// as video streams, described by CODECS and RESOLUTION and read only once
// selected (their audio is the first's); its audio renditions
// (EXT-X-MEDIA) with a playlist of their own are audio streams
// (DEFAULT=YES the default one), read alongside in decode order.
// VOD seeks by segment; live input starts three target durations from
// the end and reloads each playlist read as its segments are used up.
// Each discontinuity sequence gets its own offset onto one continuous
// timeline, shared by the renditions, whose timestamps match. AES-128
// segments are decrypted (each key fetched once); SAMPLE-AES is refused.
class HlsDemuxer : public Demuxer {
 public:
  HlsDemuxer();
  ~HlsDemuxer() override;

  // Whether |data| (the input's first bytes) is an M3U playlist.
  static bool Detect(const uint8_t* data, size_t size);

  const char* name() const override { return "hls"; }
  bool Open(const std::string& url, std::string* error) override;
  // Takes the playlist's source, already opened by the format probe.
  bool Open(std::unique_ptr<DataSource> source,
            const std::string& url,
            std::string* error);
  const MediaInfo& media_info() const override { return media_info_; }
  void SetEnabledStreams(const std::vector<int>& stream_indices) override;
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;
  void Interrupt() override;
  void Abort() override;
  std::string error() const override { return error_; }

  // Opens the playlists and segments the input refers to; OpenDataSource
  // unless set (the fuzzer serves them from memory). Set before Open().
  using Opener =
      std::function<std::unique_ptr<DataSource>(const std::string& url,
                                                std::string* error)>;
  void set_opener(Opener opener) { opener_ = std::move(opener); }

 private:
  struct Segment : HlsSegment {
    // Playlist time: from the start of the first segment known.
    int64_t start = 0;
  };
  // A media playlist and the segment open in it: a variant's, or an
  // audio rendition's.
  struct Rendition {
    std::string url;
    // Loaded: the first variant and audio renditions at the open, other
    // variants once selected. Until then a position to start from.
    bool loaded = false;
    int64_t resume = kNoTimestamp;
    // A variant's own video stream, or -1 (audio renditions).
    int video = -1;
    std::vector<Segment> segments;
    size_t current = 0;
    bool live = false;
    bool ended = false;
    int64_t target_duration = 0;
    int64_t last_reload = 0;
    bool last_reload_changed = true;
    int reload_failures = 0;
    // Only this type of stream is taken from its segments (audio
    // renditions), or any.
    MediaType only = MediaType::kUnknown;
    // The media_info_ streams it carries, in segment order.
    std::vector<int> streams;
    // The playlist time that plays at media_info_.start_time.
    int64_t origin = 0;
    bool opened = false;

    // The last map read.
    std::string map_id;
    std::vector<uint8_t> map_data;
    // The open segment: MPEG-TS through reader (from source), or fMP4
    // through mp4 (from memory). Its stream index -> media_info_ stream,
    // or -1; microseconds added to its timestamps.
    std::unique_ptr<DataSource> source;
    std::unique_ptr<TsReader> reader;
    std::unique_ptr<Mp4Demuxer> mp4;
    std::vector<int> stream_map;
    int64_t offset = 0;
    // fMP4 video for a stream described without its configuration (another
    // variant): its samples become Annex B with the parameter sets.
    int annexb_stream = -1;
    AnnexBConverter annexb;

    // The next packet, read ahead to interleave the renditions.
    EncodedPacket next;
    bool has_next = false;
    bool at_end = false;
    // Enabled while playing, from before the read position: what streams
    // it shares with the others (a variant's audio) already got is
    // dropped. Each stream's last decode time then, or empty.
    std::vector<int64_t> skip_until;
  };

  // Reads and parses the playlist at |url| through |source| (opened here
  // if null). |final_url| is where it came from after redirects.
  bool LoadPlaylist(std::unique_ptr<DataSource> source,
                    const std::string& url,
                    HlsPlaylist* playlist,
                    std::string* final_url,
                    std::string* error);
  // Takes |playlist| into |r| and opens its first segment (live: three
  // target durations from the end), which adds |r|'s streams.
  bool Start(Rendition* r, const HlsPlaylist& playlist, std::string* error);
  // Takes the playlist into |r| (live: where segments_[0] reads), and
  // goes to r->resume.
  void Load(Rendition* r, const HlsPlaylist& playlist);
  // Lists the variants other than |pick| as video streams.
  void AddVariants(const std::vector<HlsVariant>& variants, int pick);
  // Appends the segments after the last one known. True if there were.
  bool Merge(Rendition* r, const HlsPlaylist& playlist);
  // Opens r->segments[r->current]; the first one adds r's streams.
  bool OpenSegment(Rendition* r, std::string* error);
  void CloseSegment(Rendition* r);
  // Reads |length| bytes (-1: all) of |url| from |offset| (-1: 0).
  bool Fetch(const std::string& url,
             int64_t offset,
             int64_t length,
             std::vector<uint8_t>* data,
             std::string* error);
  // Decrypts |data| in place with |key|; the IV defaults to |sequence|.
  bool Decrypt(const HlsKey& key,
               int64_t sequence,
               std::vector<uint8_t>* data,
               std::string* error);
  // The segment's bytes in memory, after its map: encrypted or fMP4.
  bool LoadSegment(Rendition* r,
                   const Segment& segment,
                   std::vector<uint8_t>* data,
                   std::string* error);
  // Live: reloads r's playlist until a segment follows r->current.
  DemuxerReadResult WaitForSegments(Rendition* r);
  // The next packet of r's enabled streams.
  DemuxerReadResult ReadFrom(Rendition* r, EncodedPacket* packet);
  // Whether one of r's streams is enabled.
  bool Enabled(const Rendition& r) const;
  // Moves r to the segment that plays at |time| (media time).
  void Position(Rendition* r, int64_t time);
  // Makes |source| the one Interrupt() and Abort() reach, or none.
  void Track(DataSource* source);
  bool Failed();

  Opener opener_ = OpenDataSource;
  std::mutex mutex_;
  std::condition_variable wake_;
  DataSource* active_ = nullptr;

  // The variant played first, then audio renditions, then other variants.
  std::vector<std::unique_ptr<Rendition>> renditions_;
  // AES-128 keys by URL.
  std::map<std::string, std::array<uint8_t, 16>> keys_;
  // Discontinuity sequence -> microseconds added to its timestamps.
  std::map<int64_t, int64_t> offsets_;

  MediaInfo media_info_;
  std::vector<bool> enabled_;
  std::vector<bool> need_keyframe_;
  // The decode time of the last packet read, for a rendition enabled
  // later, and of each stream's since the last seek.
  int64_t last_dts_ = kNoTimestamp;
  std::vector<int64_t> delivered_;
  std::atomic<bool> aborted_{false};
  std::atomic<bool> interrupted_{false};
  std::string error_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_HLS_HLS_DEMUXER_H_
