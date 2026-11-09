#ifndef LYCHEE_BACKENDS_HLS_HLS_DEMUXER_H_
#define LYCHEE_BACKENDS_HLS_HLS_DEMUXER_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
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

class SegmentStream;

// HTTP Live Streaming (RFC 8216) with MPEG-TS or fragmented MP4
// (EXT-X-MAP) segments, for builds without FFmpeg. A master playlist
// plays one variant (PickHlsVariant) and lists the others it can decode
// as video streams, described by CODECS and RESOLUTION and read only once
// selected (their audio is the first's). While the first one's stream is
// selected, it moves between the variants of its codec and audio group
// by the bandwidth its segments arrived at (TS, or fMP4 with each map
// supplying the in-band parameter sets at a keyframe), or keeps to the
// one SetVariantPolicy() pins, and to those within its cap. Its audio
// renditions (EXT-X-MEDIA) with a playlist of their own are audio streams
// (DEFAULT=YES the default one), read alongside in decode order, and so
// are its WebVTT and fMP4 IMSC text/image and WebVTT subtitle renditions.
// VOD seeks by segment; live input starts three target durations from
// the end and reloads each playlist read as its segments are used up.
// A live playlist that keeps a window (EVENT, or at least
// kLiveSeekableTargetDurations long when opened) seeks within it:
// positions count from its first segment at the open.
// Each discontinuity sequence gets its own offset onto one continuous
// timeline, shared by the renditions, whose timestamps match. AES-128
// segments are decrypted through the platform (CommonCrypto, BCrypt or
// javax.crypto, each key fetched once); SAMPLE-AES is refused.
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
  int64_t download_rate() const override {
    return estimate_ > 0 ? estimate_ : -1;
  }
  std::vector<VariantInfo> variants() const override { return variants_; }
  int current_variant() const override { return current_variant_; }
  void SetVariantPolicy(int pinned, int64_t max_bandwidth) override;
  bool live_window(LiveWindow* window) const override;

  // Opens the playlists and segments the input refers to; a shared
  // opener (CreateSharedOpener) unless set (the fuzzer serves them from
  // memory). Set before Open().
  using Opener = DataSourceOpener;
  void set_opener(Opener opener) { opener_ = std::move(opener); }
  // The time between live reloads: MonotonicMicros() and a wait until
  // |until| unless set (the fuzzer advances a clock of its own). |wait|
  // returns false to give up, as an abort does. Set before Open().
  struct Clock {
    std::function<int64_t()> now;
    std::function<bool(int64_t until)> wait;
  };
  void set_clock(Clock clock) { clock_ = std::move(clock); }
  // Makes Open() fail for a master playlist with subtitle renditions
  // that this demuxer cannot read: for builds
  // with a demuxer that reads them (FFmpeg's). Set before Open().
  void set_refuse_subtitles(bool refuse) { refuse_subtitles_ = refuse; }

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
    // A variant's own video stream, or -1 (audio renditions), and the
    // variant it reads (variants_; -1 without a master playlist).
    int video = -1;
    int variant = -1;
    std::vector<Segment> segments;
    size_t current = 0;
    bool live = false;
    bool ended = false;
    bool event = false;
    // The sequence number of the first segment the last playlist loaded
    // listed: those before it are gone from the server.
    int64_t listed_from = 0;
    int64_t target_duration = 0;
    int64_t last_reload = 0;
    bool last_reload_changed = true;
    int reload_failures = 0;
    // Only this type of stream is taken from its segments (audio and
    // subtitle renditions), or any.
    MediaType only = MediaType::kUnknown;
    // The media_info_ streams it carries, in segment order.
    std::vector<int> streams;
    // The playlist time that plays at media_info_.start_time.
    int64_t origin = 0;
    bool opened = false;

    // The last map read.
    std::string map_id;
    std::shared_ptr<const std::vector<uint8_t>> map_data;
    // The open segment: MPEG-TS through reader, or fMP4 through mp4 (from
    // stream, the map and source's media). source is the network's
    // (streamed), or the segment in memory (decrypted). Its stream index
    // -> media_info_ stream, or -1; microseconds added to its timestamps.
    std::unique_ptr<DataSource> source;
    bool streamed = false;
    std::unique_ptr<TsReader> reader;
    std::unique_ptr<Mp4Demuxer> mp4;
    SegmentStream* stream = nullptr;
    std::vector<int> stream_map;
    int64_t offset = 0;
    // fMP4 video for a stream described without its configuration (another
    // variant): its samples become Annex B with the parameter sets.
    int annexb_stream = -1;
    AnnexBConverter annexb;
    // Once most of the open segment arrived: measured (the first variant)
    // and the next one's request made.
    bool ahead = false;
    bool measured = false;
    // That request: the source of the segment with this URL and number,
    // opened while the one before is read, so that its first bytes come
    // without waiting for a new request at the segment's end. The next
    // byte range of the same resource reads on in the open segment's
    // source instead.
    std::unique_ptr<DataSource> prefetch;
    std::string prefetch_url;
    int64_t prefetch_sequence = -1;
    // A WebVTT segment: its cues, as packets of the rendition's stream.
    bool text_open = false;
    std::deque<EncodedPacket> text;

    // The next packet, read ahead to interleave the renditions, and the
    // variant it was read from.
    EncodedPacket next;
    int next_variant = -1;
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
  // Live: the last segment that starts three target durations or more
  // before the end of r's playlist, or the first.
  size_t LiveStart(const Rendition& r) const;
  // Live: window_ from the first variant's playlist.
  void UpdateWindow(const Rendition& r);
  // Seek() on live input: reloads the playlists read, so that the target
  // is placed in their current windows.
  void ReloadForSeek();
  // Opens r->segments[r->current]; the first one adds r's streams.
  bool OpenSegment(Rendition* r, std::string* error);
  // OpenSegment() for a subtitle rendition.
  bool OpenWebVtt(Rendition* r, std::string* error);
  // Also drops r->prefetch unless |keep_prefetch|.
  void CloseSegment(Rendition* r, bool keep_prefetch = false);
  // Appends |length| bytes (-1: all) of |url| from |offset| (-1: 0) to
  // |data|, and the source's transfer_rate() into |rate| if given. Reads
  // |source| if given, else opens |url|.
  bool Fetch(const std::string& url,
             int64_t offset,
             int64_t length,
             std::vector<uint8_t>* data,
             std::string* error,
             int64_t* rate = nullptr,
             std::unique_ptr<DataSource> source = nullptr);
  // Decrypts |data| from |from| on with |key|; the IV defaults to
  // |sequence|.
  bool Decrypt(const HlsKey& key,
               int64_t sequence,
               std::vector<uint8_t>* data,
               size_t from,
               std::string* error);
  // Loads the segment's map into r->map_data unless it is the last read.
  bool LoadMap(Rendition* r, const Segment& segment, std::string* error);
  // The segment's bytes in memory, after its map: encrypted or fMP4.
  bool LoadSegment(Rendition* r,
                   const Segment& segment,
                   std::vector<uint8_t>* data,
                   std::string* error);
  // Takes a segment's transfer rate (bit/s) into the estimate, unless
  // too few bytes measure it (false).
  bool Measure(int64_t rate, int64_t bytes);
  // The rung of ladder_ the estimate affords r (the first variant) within
  // the cap, or the one pinned; rung_ while it has nothing to move
  // between.
  size_t AffordedRung(const Rendition& r) const;
  // Before the first variant opens its next segment: moves it to the
  // variant the estimate affords, if another.
  void Adapt(Rendition* r);
  // Once most of r's open segment arrived: measures it and requests the
  // next one (r->prefetch), unless a variant switch comes first. Not
  // before Open() returns, which leaves it to a later read.
  void Prefetch(Rendition* r);
  // r->prefetch if it is |segment|'s; dropped otherwise.
  std::unique_ptr<DataSource> TakePrefetch(Rendition* r,
                                           const Segment& segment);
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

  int64_t Now() const;

  Opener opener_ = CreateSharedOpener();
  // Open() has returned: the variants to adapt between are known.
  bool started_ = false;
  bool refuse_subtitles_ = false;
  Clock clock_;
  std::mutex mutex_;
  std::condition_variable wake_;
  DataSource* active_ = nullptr;

  // The variant played first, then audio renditions, then other variants.
  std::vector<std::unique_ptr<Rendition>> renditions_;
  // What the first variant moves between, by bandwidth from the lowest,
  // and the one it reads; the bandwidth estimate (bit/s), 0 until a
  // segment was measured.
  std::vector<HlsVariant> ladder_;
  size_t rung_ = 0;
  int64_t estimate_ = 0;
  // The master playlist's variants this build plays, in its order, and
  // their URLs.
  std::vector<VariantInfo> variants_;
  std::vector<std::string> variant_urls_;
  // SetVariantPolicy().
  std::atomic<int> pinned_{-1};
  std::atomic<int64_t> max_bandwidth_{0};
  // Of the last video packet Read() returned.
  int current_variant_ = -1;
  // Live input that can seek: the window, under its own lock.
  mutable std::mutex window_mutex_;
  LiveWindow window_;
  bool has_window_ = false;
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
