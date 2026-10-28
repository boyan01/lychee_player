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
#include "media/data_source.h"
#include "media/demuxer.h"

namespace lychee {

// HTTP Live Streaming (RFC 8216) with MPEG-TS or fragmented MP4
// (EXT-X-MAP) segments, for builds without FFmpeg. A master playlist
// plays one variant (PickHlsVariant), without switching. VOD seeks by
// segment; live input starts three target durations from the end and
// reloads the playlist as segments are used up. Each discontinuity
// sequence gets its own offset onto one continuous timeline. AES-128
// segments are decrypted (each key fetched once); SAMPLE-AES is refused.
// Alternative audio renditions (EXT-X-MEDIA) are not played.
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

  // Reads and parses the playlist at |url| through |source| (opened here
  // if null). |final_url| is where it came from after redirects.
  bool LoadPlaylist(std::unique_ptr<DataSource> source,
                    const std::string& url,
                    HlsPlaylist* playlist,
                    std::string* final_url,
                    std::string* error);
  // Appends the segments after the last one known. True if there were.
  bool Merge(const HlsPlaylist& playlist);
  // Opens segments_[current_]; the first one also fills media_info_.
  bool OpenSegment(std::string* error);
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
  bool LoadSegment(const Segment& segment,
                   std::vector<uint8_t>* data,
                   std::string* error);
  void CloseSegment();
  // Live: reloads the playlist until a segment follows current_.
  DemuxerReadResult WaitForSegments();
  // Makes |source| the one Interrupt() and Abort() reach, or none.
  void Track(DataSource* source);
  bool Failed();

  Opener opener_ = OpenDataSource;
  std::mutex mutex_;
  std::condition_variable wake_;
  DataSource* active_ = nullptr;

  std::string playlist_url_;
  std::vector<Segment> segments_;
  size_t current_ = 0;
  bool live_ = false;
  bool ended_ = false;
  int64_t target_duration_ = 0;
  int64_t last_reload_ = 0;
  bool last_reload_changed_ = true;
  int reload_failures_ = 0;

  // AES-128 keys by URL, and the last map read.
  std::map<std::string, std::array<uint8_t, 16>> keys_;
  std::string map_id_;
  std::vector<uint8_t> map_data_;

  // The open segment: MPEG-TS through segment_reader_ (from
  // segment_source_), or fMP4 through segment_mp4_ (from memory).
  std::unique_ptr<DataSource> segment_source_;
  std::unique_ptr<TsReader> segment_reader_;
  std::unique_ptr<Mp4Demuxer> segment_mp4_;
  // The segment's stream index -> media_info_ stream, or -1.
  std::vector<int> stream_map_;
  // Discontinuity sequence -> microseconds added to its timestamps.
  std::map<int64_t, int64_t> offsets_;
  int64_t offset_ = 0;
  // The playlist time that plays at media_info_.start_time.
  int64_t origin_ = 0;

  MediaInfo media_info_;
  std::vector<bool> enabled_;
  std::vector<bool> need_keyframe_;
  std::atomic<bool> aborted_{false};
  std::atomic<bool> interrupted_{false};
  std::string error_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_HLS_HLS_DEMUXER_H_
