#define LOG_TAG "mp4"

#include "backends/mp4/mp4_demuxer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <optional>

#include "base/log.h"

namespace lychee {

namespace {

constexpr uint32_t FourCC(const char (&code)[5]) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(code[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(code[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(code[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(code[3]));
}

// The moov box of a file with hours of video is a few MB; anything far
// larger is not a file this demuxer can read.
constexpr uint64_t kMaxMoovSize = 256 << 20;
// Samples in all tracks: 77 hours of 60 fps video. Each takes a Sample.
constexpr size_t kMaxSamples = 1 << 24;
// One sample: an 8K intra frame is a few MB. A larger size is a corrupt
// table, and allocating it could take the process down.
constexpr uint32_t kMaxSampleSize = 64 << 20;

// Big-endian fields with bounds checks: reads past the end yield zeros
// and clear ok().
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool ok() const { return ok_; }
  size_t position() const { return position_; }
  size_t remaining() const { return size_ - position_; }
  const uint8_t* current() const { return data_ + position_; }

  uint64_t Bits(int bytes) {
    if (remaining() < static_cast<size_t>(bytes)) {
      ok_ = false;
      position_ = size_;
      return 0;
    }
    uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) {
      value = (value << 8) | data_[position_++];
    }
    return value;
  }
  uint8_t U8() { return static_cast<uint8_t>(Bits(1)); }
  uint16_t U16() { return static_cast<uint16_t>(Bits(2)); }
  uint32_t U32() { return static_cast<uint32_t>(Bits(4)); }
  uint64_t U64() { return Bits(8); }
  void Skip(size_t bytes) {
    if (remaining() < bytes) {
      ok_ = false;
      position_ = size_;
    } else {
      position_ += bytes;
    }
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t position_ = 0;
  bool ok_ = true;
};

struct Box {
  uint32_t type = 0;
  // From the box's first byte; |payload| is after the header.
  const uint8_t* start = nullptr;
  const uint8_t* payload = nullptr;
  size_t size = 0;
  size_t payload_size = 0;
};

std::vector<Box> Children(const uint8_t* data, size_t size) {
  std::vector<Box> boxes;
  Reader reader(data, size);
  while (reader.remaining() >= 8) {
    Box box;
    box.start = reader.current();
    uint64_t box_size = reader.U32();
    box.type = reader.U32();
    size_t header = 8;
    if (box_size == 1) {
      box_size = reader.U64();
      header = 16;
    } else if (box_size == 0) {
      box_size = reader.remaining() + 8;
    }
    if (!reader.ok() || box_size < header ||
        box_size - header > reader.remaining()) {
      break;
    }
    box.payload = reader.current();
    box.size = box_size;
    box.payload_size = box_size - header;
    reader.Skip(box.payload_size);
    boxes.push_back(box);
  }
  return boxes;
}

std::optional<Box> Find(const std::vector<Box>& boxes, uint32_t type) {
  for (const Box& box : boxes) {
    if (box.type == type) {
      return box;
    }
  }
  return std::nullopt;
}

// |ticks| / |timescale| seconds in microseconds, without overflow.
int64_t Rescale(int64_t ticks, int64_t timescale) {
  if (timescale <= 0) {
    return 0;
  }
  return ticks / timescale * kMicrosPerSecond +
         ticks % timescale * kMicrosPerSecond / timescale;
}

std::string FourCCName(uint32_t type) {
  std::string name;
  for (int shift = 24; shift >= 0; shift -= 8) {
    char c = static_cast<char>((type >> shift) & 0xff);
    name.push_back(c == ' ' ? '_' : c);
  }
  return name;
}

// An MPEG-4 descriptor's tag and length (ISO/IEC 14496-1); the reader is
// then at its body.
bool ReadDescriptor(Reader* reader, uint8_t* tag, size_t* length) {
  *tag = reader->U8();
  *length = 0;
  for (int i = 0; i < 4; ++i) {
    uint8_t byte = reader->U8();
    *length = (*length << 7) | (byte & 0x7f);
    if (!(byte & 0x80)) {
      break;
    }
  }
  return reader->ok() && *length <= reader->remaining();
}

// Fills |config| from an esds box's payload.
void ParseEsds(const Box& esds, AudioConfig* config) {
  Reader reader(esds.payload, esds.payload_size);
  reader.Skip(4);  // version, flags
  uint8_t tag;
  size_t length;
  if (!ReadDescriptor(&reader, &tag, &length) || tag != 0x03) {
    return;
  }
  reader.Skip(2);  // ES_ID
  uint8_t flags = reader.U8();
  if (flags & 0x80) {
    reader.Skip(2);  // dependsOn_ES_ID
  }
  if (flags & 0x40) {
    reader.Skip(reader.U8());  // URL
  }
  if (flags & 0x20) {
    reader.Skip(2);  // OCR_ES_Id
  }
  if (!ReadDescriptor(&reader, &tag, &length) || tag != 0x04) {
    return;
  }
  uint8_t object_type = reader.U8();
  reader.Skip(12);  // streamType, bufferSizeDB, maxBitrate, avgBitrate
  switch (object_type) {
    case 0x40:  // MPEG-4 audio
    case 0x66:  // MPEG-2 AAC profiles
    case 0x67:
    case 0x68:
      config->codec = AudioCodec::kAAC;
      config->codec_name = "aac";
      break;
    case 0x69:  // MPEG-2 and MPEG-1 audio
    case 0x6b:
      config->codec = AudioCodec::kMP3;
      config->codec_name = "mp3";
      break;
    default:
      return;
  }
  if (ReadDescriptor(&reader, &tag, &length) && tag == 0x05) {
    config->extradata.assign(reader.current(), reader.current() + length);
  }
}

void ParseVideoEntry(const Box& entry, VideoConfig* config) {
  Reader reader(entry.payload, entry.payload_size);
  // reserved, data_reference_index, pre_defined, reserved, pre_defined.
  reader.Skip(24);
  config->width = reader.U16();
  config->height = reader.U16();
  // resolutions, reserved, frame_count, compressorname, depth,
  // pre_defined.
  reader.Skip(50);
  if (!reader.ok()) {
    return;
  }
  switch (entry.type) {
    case FourCC("avc1"):
    case FourCC("avc3"):
      config->codec = VideoCodec::kH264;
      config->codec_name = "h264";
      break;
    case FourCC("hvc1"):
    case FourCC("hev1"):
      config->codec = VideoCodec::kHEVC;
      config->codec_name = "hevc";
      break;
    default:
      config->codec = VideoCodec::kOther;
      config->codec_name = FourCCName(entry.type);
      break;
  }
  for (const Box& child : Children(reader.current(), reader.remaining())) {
    if (child.type == FourCC("avcC") || child.type == FourCC("hvcC")) {
      config->extradata.assign(child.payload,
                               child.payload + child.payload_size);
    } else if (child.type == FourCC("colr")) {
      Reader colr(child.payload, child.payload_size);
      uint32_t kind = colr.U32();
      if (kind == FourCC("nclx") || kind == FourCC("nclc")) {
        uint16_t primaries = colr.U16();
        uint16_t transfer = colr.U16();
        uint16_t matrix = colr.U16();
        bool full_range = kind == FourCC("nclx") && (colr.U8() & 0x80);
        if (colr.ok()) {
          config->color = {static_cast<uint8_t>(primaries),
                           static_cast<uint8_t>(transfer),
                           static_cast<uint8_t>(matrix), full_range};
        }
      }
    } else if (child.type == FourCC("pasp")) {
      Reader pasp(child.payload, child.payload_size);
      uint32_t h_spacing = pasp.U32();
      uint32_t v_spacing = pasp.U32();
      if (pasp.ok() && h_spacing > 0 && v_spacing > 0) {
        config->sar_num = static_cast<int>(h_spacing);
        config->sar_den = static_cast<int>(v_spacing);
      }
    }
  }
}

void ParseAudioEntry(const Box& entry, AudioConfig* config) {
  Reader reader(entry.payload, entry.payload_size);
  reader.Skip(8);  // reserved, data_reference_index
  uint16_t version = reader.U16();
  reader.Skip(6);  // revision, vendor
  config->channels = reader.U16();
  reader.Skip(6);  // samplesize, compression ID, packet size
  config->sample_rate = static_cast<int>(reader.U32() >> 16);
  if (version == 1) {
    reader.Skip(16);
  } else if (version == 2) {
    // QuickTime's sound description v2: the rate as a double, then the
    // channel count.
    reader.Skip(4);
    uint64_t bits = reader.U64();
    double rate;
    memcpy(&rate, &bits, sizeof(rate));
    config->sample_rate = static_cast<int>(rate);
    config->channels = static_cast<int>(reader.U32());
    reader.Skip(20);
  }
  if (!reader.ok()) {
    return;
  }
  config->codec = AudioCodec::kOther;
  config->codec_name = FourCCName(entry.type);
  std::vector<Box> children = Children(reader.current(), reader.remaining());
  if (entry.type == FourCC("mp4a")) {
    std::optional<Box> esds = Find(children, FourCC("esds"));
    if (!esds) {
      // QuickTime puts it in a wave box.
      if (std::optional<Box> wave = Find(children, FourCC("wave"))) {
        esds = Find(Children(wave->payload, wave->payload_size),
                    FourCC("esds"));
        if (esds) {
          ParseEsds(*esds, config);
        }
        return;
      }
    }
    if (esds) {
      ParseEsds(*esds, config);
    }
  } else if (entry.type == FourCC("alac")) {
    config->codec = AudioCodec::kALAC;
    config->codec_name = "alac";
    // The whole inner alac box, header included, as FFmpeg's extradata
    // and AudioToolbox's cookie have it.
    if (std::optional<Box> alac = Find(children, FourCC("alac"))) {
      config->extradata.assign(alac->start, alac->start + alac->size);
    }
  }
}

std::string Language(uint16_t packed) {
  std::string language;
  for (int shift = 10; shift >= 0; shift -= 5) {
    language.push_back(static_cast<char>(((packed >> shift) & 0x1f) + 0x60));
  }
  return language;
}

}  // namespace

Mp4Demuxer::Mp4Demuxer() = default;
Mp4Demuxer::~Mp4Demuxer() = default;

bool Mp4Demuxer::Open(const std::string& url, std::string* error) {
  std::unique_ptr<DataSource> source = OpenDataSource(url, error);
  if (!source) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(source_mutex_);
    source_ = std::move(source);
    if (aborted_) {
      source_->Interrupt();
    }
  }
  // Top-level boxes up to moov. With moov at the end (no faststart) this
  // reads two box headers per mdat, not the media.
  int64_t file_size = -1;
  int64_t offset = 0;
  bool first = true;
  for (;;) {
    if (aborted_) {
      *error = "aborted";
      return false;
    }
    uint8_t header[16];
    int64_t got = source_->Read(offset, header, sizeof(header));
    if (got < 8) {
      *error = got < 0 ? source_->error() : "no moov box: not an MP4 file";
      return false;
    }
    if (first) {
      // Known once the first bytes arrived (HTTP).
      file_size = source_->size();
    }
    Reader reader(header, static_cast<size_t>(got));
    uint64_t size = reader.U32();
    uint32_t type = reader.U32();
    uint64_t header_size = 8;
    if (size == 1) {
      size = reader.U64();
      header_size = 16;
    } else if (size == 0 && file_size > 0) {
      size = static_cast<uint64_t>(file_size - offset);
    }
    if (first) {
      static const uint32_t kFirst[] = {FourCC("ftyp"), FourCC("moov"),
                                        FourCC("free"), FourCC("wide"),
                                        FourCC("mdat"), FourCC("skip")};
      if (std::find(std::begin(kFirst), std::end(kFirst), type) ==
          std::end(kFirst)) {
        *error = "not an MP4 file";
        return false;
      }
      first = false;
    }
    if (!reader.ok() || size < header_size) {
      *error = "malformed MP4 box";
      return false;
    }
    if (type == FourCC("moof")) {
      *error = "fragmented MP4 needs FFmpeg";
      return false;
    }
    if (type == FourCC("moov")) {
      if (size > kMaxMoovSize) {
        *error = "moov box too large";
        return false;
      }
      std::vector<uint8_t> moov(size - header_size);
      int64_t position = offset + static_cast<int64_t>(header_size);
      size_t filled = 0;
      while (filled < moov.size()) {
        int64_t n = source_->Read(position + static_cast<int64_t>(filled),
                                  moov.data() + filled, moov.size() - filled);
        if (n <= 0) {
          *error = n < 0 ? source_->error() : "truncated moov box";
          return false;
        }
        filled += static_cast<size_t>(n);
      }
      file_size_ = file_size;
      try {
        if (!ParseMoov(moov.data(), moov.size(), error)) {
          return false;
        }
      } catch (const std::bad_alloc&) {
        *error = "out of memory for the sample tables";
        return false;
      }
      media_info_.seekable = file_size > 0;
      return true;
    }
    offset += static_cast<int64_t>(size);
  }
}

bool Mp4Demuxer::ParseMoov(const uint8_t* data, size_t size, std::string* error) {
  std::vector<Box> boxes = Children(data, size);
  if (Find(boxes, FourCC("mvex"))) {
    *error = "fragmented MP4 needs FFmpeg";
    return false;
  }
  std::optional<Box> mvhd = Find(boxes, FourCC("mvhd"));
  if (!mvhd) {
    *error = "no mvhd box";
    return false;
  }
  Reader header(mvhd->payload, mvhd->payload_size);
  int version = header.U8();
  header.Skip(3 + (version == 1 ? 16 : 8));  // flags, creation, modification
  int64_t timescale = header.U32();
  int64_t duration =
      static_cast<int64_t>(version == 1 ? header.U64() : header.U32());
  if (!header.ok() || timescale <= 0) {
    *error = "malformed mvhd box";
    return false;
  }
  media_info_ = MediaInfo();
  media_info_.format_name = "mp4";
  tracks_.clear();
  sample_budget_ = kMaxSamples;
  for (const Box& box : boxes) {
    if (box.type == FourCC("trak")) {
      ParseTrack(box.payload, box.payload_size, timescale);
    } else if (box.type == FourCC("udta")) {
      ParseMetadata(box.payload, box.payload_size);
    } else if (box.type == FourCC("meta")) {
      ParseMetadata(box.start, box.size);
    }
  }
  if (tracks_.empty()) {
    *error = "no video or audio track";
    return false;
  }
  if (duration > 0 && duration != (version == 1 ? -1 : 0xffffffff)) {
    media_info_.duration = Rescale(duration, timescale);
  } else {
    for (const StreamInfo& stream : media_info_.streams) {
      media_info_.duration = std::max(media_info_.duration, stream.duration);
    }
  }
  int64_t start = kInfiniteTime;
  for (const Track& track : tracks_) {
    if (!track.samples.empty()) {
      start = std::min(start, track.samples.front().pts);
    }
  }
  // Audio priming (an edit list that skips the encoder delay) starts
  // before zero; the presentation does not.
  media_info_.start_time = start == kInfiniteTime ? 0 : std::max<int64_t>(start, 0);
  return true;
}

bool Mp4Demuxer::ParseTrack(const uint8_t* data,
                            size_t size,
                            int64_t movie_timescale) {
  std::vector<Box> trak = Children(data, size);
  std::optional<Box> tkhd = Find(trak, FourCC("tkhd"));
  std::optional<Box> mdia = Find(trak, FourCC("mdia"));
  if (!tkhd || !mdia) {
    return false;
  }
  std::vector<Box> media = Children(mdia->payload, mdia->payload_size);
  std::optional<Box> mdhd = Find(media, FourCC("mdhd"));
  std::optional<Box> hdlr = Find(media, FourCC("hdlr"));
  std::optional<Box> minf = Find(media, FourCC("minf"));
  if (!mdhd || !hdlr || !minf) {
    return false;
  }
  std::optional<Box> stbl = Find(Children(minf->payload, minf->payload_size),
                         FourCC("stbl"));
  if (!stbl) {
    return false;
  }

  StreamInfo info;
  Reader handler(hdlr->payload, hdlr->payload_size);
  handler.Skip(8);  // version, flags, pre_defined
  uint32_t handler_type = handler.U32();
  if (handler_type == FourCC("vide")) {
    info.type = MediaType::kVideo;
  } else if (handler_type == FourCC("soun")) {
    info.type = MediaType::kAudio;
  } else {
    return false;
  }

  Reader track_header(tkhd->payload, tkhd->payload_size);
  int version = track_header.U8();
  uint32_t flags = static_cast<uint32_t>(track_header.Bits(3));
  // creation, modification, track_ID, reserved, duration, reserved, layer,
  // alternate_group, volume, reserved.
  track_header.Skip(version == 1 ? 32 : 20);
  track_header.Skip(16);
  int32_t matrix[4];
  for (int i = 0; i < 4; ++i) {
    matrix[i] = static_cast<int32_t>(track_header.U32());
    if (i == 1) {
      track_header.Skip(4);  // u
    }
  }
  info.default_track = (flags & 1) != 0;

  Reader media_header(mdhd->payload, mdhd->payload_size);
  version = media_header.U8();
  media_header.Skip(3 + (version == 1 ? 16 : 8));
  const int64_t timescale = media_header.U32();
  int64_t media_duration = static_cast<int64_t>(
      version == 1 ? media_header.U64() : media_header.U32());
  uint16_t language = media_header.U16();
  if (!media_header.ok() || timescale <= 0) {
    return false;
  }
  info.language = Language(language);

  std::vector<Box> table = Children(stbl->payload, stbl->payload_size);
  std::optional<Box> stsd = Find(table, FourCC("stsd"));
  if (!stsd) {
    return false;
  }
  Reader descriptions(stsd->payload, stsd->payload_size);
  descriptions.Skip(8);  // version, flags, entry_count
  std::vector<Box> entries =
      Children(descriptions.current(), descriptions.remaining());
  if (entries.empty()) {
    return false;
  }
  if (info.type == MediaType::kVideo) {
    ParseVideoEntry(entries[0], &info.video);
    // The matrix maps (x, y) to (a x + c y, b x + d y): a rotation by
    // atan2(b, a), clockwise on screen, where y points down.
    double angle = std::atan2(static_cast<double>(matrix[1]),
                              static_cast<double>(matrix[0])) *
                   180.0 / 3.14159265358979323846;
    int degrees = static_cast<int>(std::lround(angle / 90.0)) * 90;
    info.video.rotation = ((degrees % 360) + 360) % 360;
  } else {
    ParseAudioEntry(entries[0], &info.audio);
  }

  // Edit list: leading empty edits delay the track; the first media edit
  // says where in the media it starts.
  int64_t empty = 0;
  int64_t media_time = 0;
  if (std::optional<Box> edts = Find(trak, FourCC("edts"))) {
    if (std::optional<Box> elst =
            Find(Children(edts->payload, edts->payload_size), FourCC("elst"))) {
      Reader edits(elst->payload, elst->payload_size);
      int edits_version = edits.U8();
      edits.Skip(3);
      uint32_t count = edits.U32();
      for (uint32_t i = 0; i < count && edits.ok(); ++i) {
        int64_t segment = static_cast<int64_t>(edits.Bits(edits_version == 1 ? 8 : 4));
        int64_t time = edits_version == 1
                           ? static_cast<int64_t>(edits.U64())
                           : static_cast<int32_t>(edits.U32());
        edits.Skip(4);  // media_rate
        if (time == -1) {
          empty += Rescale(segment, movie_timescale);
        } else {
          media_time = time;
          break;
        }
      }
    }
  }

  // Sample table: decode deltas, composition offsets, chunk layout, sizes,
  // chunk offsets and sync samples.
  auto entries_of = [&](uint32_t type, size_t header) -> Reader {
    std::optional<Box> box = Find(table, type);
    if (!box || box->payload_size < header) {
      return Reader(nullptr, 0);
    }
    return Reader(box->payload, box->payload_size);
  };
  std::vector<int64_t> chunk_offsets;
  if (Reader stco = entries_of(FourCC("stco"), 8); stco.remaining()) {
    stco.Skip(4);
    uint32_t count = stco.U32();
    for (uint32_t i = 0; i < count && stco.ok(); ++i) {
      chunk_offsets.push_back(stco.U32());
    }
  } else if (Reader co64 = entries_of(FourCC("co64"), 8); co64.remaining()) {
    co64.Skip(4);
    uint32_t count = co64.U32();
    for (uint32_t i = 0; i < count && co64.ok(); ++i) {
      chunk_offsets.push_back(static_cast<int64_t>(co64.U64()));
    }
  }
  std::vector<uint32_t> sizes;
  if (Reader stsz = entries_of(FourCC("stsz"), 12); stsz.remaining()) {
    stsz.Skip(4);
    uint32_t fixed = stsz.U32();
    uint32_t count = stsz.U32();
    if (fixed) {
      // No table backs the count: bound it by the bytes it claims, or a few
      // bytes of a file ask for gigabytes.
      if (count > sample_budget_ ||
          (file_size_ > 0 &&
           uint64_t{count} * fixed > static_cast<uint64_t>(file_size_))) {
        LOGW("track %zu: %u samples of %u bytes do not fit; skipped",
             tracks_.size(), count, fixed);
        return false;
      }
      sizes.assign(count, fixed);
    } else {
      for (uint32_t i = 0; i < count && stsz.ok(); ++i) {
        sizes.push_back(stsz.U32());
      }
    }
  } else if (Reader stz2 = entries_of(FourCC("stz2"), 12); stz2.remaining()) {
    stz2.Skip(7);
    int field = stz2.U8();
    uint32_t count = stz2.U32();
    for (uint32_t i = 0; i < count && stz2.ok(); ++i) {
      if (field == 4) {
        uint8_t pair = stz2.U8();
        sizes.push_back(pair >> 4);
        if (++i < count) {
          sizes.push_back(pair & 0xf);
        }
      } else {
        sizes.push_back(static_cast<uint32_t>(stz2.Bits(field / 8)));
      }
    }
  }
  const size_t count = sizes.size();
  if (count > sample_budget_) {
    LOGW("track %zu: too many samples (%zu); skipped", tracks_.size(), count);
    return false;
  }
  sample_budget_ -= count;
  Track track;
  track.samples.resize(count);

  Reader stsc = entries_of(FourCC("stsc"), 8);
  stsc.Skip(4);
  uint32_t runs = stsc.U32();
  std::vector<std::pair<uint32_t, uint32_t>> chunk_runs;  // first, samples
  for (uint32_t i = 0; i < runs && stsc.ok(); ++i) {
    uint32_t first_chunk = stsc.U32();
    uint32_t samples_per_chunk = stsc.U32();
    stsc.Skip(4);
    if (stsc.ok() && first_chunk > 0) {
      chunk_runs.emplace_back(first_chunk - 1, samples_per_chunk);
    }
  }
  size_t sample = 0;
  for (size_t run = 0; run < chunk_runs.size() && sample < count; ++run) {
    size_t end = run + 1 < chunk_runs.size() ? chunk_runs[run + 1].first
                                             : chunk_offsets.size();
    for (size_t chunk = chunk_runs[run].first;
         chunk < end && chunk < chunk_offsets.size() && sample < count;
         ++chunk) {
      uint64_t offset = static_cast<uint64_t>(chunk_offsets[chunk]);
      for (uint32_t i = 0; i < chunk_runs[run].second && sample < count; ++i) {
        track.samples[sample].offset = offset;
        track.samples[sample].size = sizes[sample];
        offset += sizes[sample];
        ++sample;
      }
    }
  }
  if (sample < count) {
    LOGW("track %zu: the chunk table covers %zu of %zu samples",
         tracks_.size(), sample, count);
    track.samples.resize(sample);
  }

  Reader stts = entries_of(FourCC("stts"), 8);
  stts.Skip(4);
  uint32_t deltas = stts.U32();
  int64_t dts = 0;
  sample = 0;
  for (uint32_t i = 0; i < deltas && stts.ok() && sample < track.samples.size(); ++i) {
    uint32_t run = stts.U32();
    uint32_t delta = stts.U32();
    for (uint32_t j = 0; j < run && sample < track.samples.size(); ++j) {
      Sample& s = track.samples[sample++];
      s.dts = dts;
      s.duration = delta;
      dts += delta;
    }
  }
  for (; sample < track.samples.size(); ++sample) {
    track.samples[sample].dts = dts;
    track.samples[sample].duration = 0;
  }
  std::vector<int32_t> composition(track.samples.size(), 0);
  if (Reader ctts = entries_of(FourCC("ctts"), 8); ctts.remaining()) {
    ctts.Skip(4);
    uint32_t offsets = ctts.U32();
    sample = 0;
    for (uint32_t i = 0; i < offsets && ctts.ok() && sample < composition.size(); ++i) {
      uint32_t run = ctts.U32();
      // Signed in version 1, and in practice in version 0 too.
      auto offset = static_cast<int32_t>(ctts.U32());
      for (uint32_t j = 0; j < run && sample < composition.size(); ++j) {
        composition[sample++] = offset;
      }
    }
  }
  bool all_sync = true;
  if (Reader stss = entries_of(FourCC("stss"), 8); stss.remaining()) {
    all_sync = false;
    stss.Skip(4);
    uint32_t syncs = stss.U32();
    for (Sample& s : track.samples) {
      s.sync = false;
    }
    for (uint32_t i = 0; i < syncs && stss.ok(); ++i) {
      uint32_t number = stss.U32();
      if (number > 0 && number <= track.samples.size()) {
        track.samples[number - 1].sync = true;
      }
    }
  }
  for (size_t i = 0; i < track.samples.size(); ++i) {
    Sample& s = track.samples[i];
    if (all_sync) {
      s.sync = true;
    }
    s.pts = Rescale(s.dts + composition[i] - media_time, timescale) + empty;
    s.dts = Rescale(s.dts - media_time, timescale) + empty;
    s.duration = Rescale(s.duration, timescale);
  }
  if (track.samples.empty()) {
    return false;
  }

  info.index = static_cast<int>(media_info_.streams.size());
  info.duration = Rescale(media_duration, timescale);
  if (info.type == MediaType::kVideo && media_info_.default_video_stream < 0) {
    media_info_.default_video_stream = info.index;
  } else if (info.type == MediaType::kAudio &&
             media_info_.default_audio_stream < 0) {
    media_info_.default_audio_stream = info.index;
  }
  media_info_.streams.push_back(std::move(info));
  tracks_.push_back(std::move(track));
  return true;
}

void Mp4Demuxer::ParseMetadata(const uint8_t* data, size_t size) {
  for (const Box& box : Children(data, size)) {
    if (box.type == FourCC("meta")) {
      // A full box in MP4, a plain one in QuickTime.
      // Its first child is hdlr: after version and flags in a full box.
      Reader probe(box.payload, box.payload_size);
      probe.Skip(8);
      bool full = probe.U32() == FourCC("hdlr");
      size_t skip = full ? 4 : 0;
      if (box.payload_size > skip) {
        ParseMetadata(box.payload + skip, box.payload_size - skip);
      }
      continue;
    }
    if (box.type != FourCC("ilst")) {
      continue;
    }
    for (const Box& item : Children(box.payload, box.payload_size)) {
      std::optional<Box> value =
          Find(Children(item.payload, item.payload_size), FourCC("data"));
      if (!value || value->payload_size < 8) {
        continue;
      }
      // type indicator, locale.
      const uint8_t* bytes = value->payload + 8;
      size_t length = value->payload_size - 8;
      if (item.type == FourCC("covr")) {
        if (!media_info_.artwork) {
          media_info_.artwork =
              std::make_shared<const std::vector<uint8_t>>(bytes, bytes + length);
        }
        continue;
      }
      static const std::pair<uint32_t, const char*> kKeys[] = {
          {FourCC("\251nam"), "title"},   {FourCC("\251ART"), "artist"},
          {FourCC("\251alb"), "album"},   {FourCC("aART"), "album_artist"},
          {FourCC("\251day"), "date"},    {FourCC("\251gen"), "genre"},
          {FourCC("\251cmt"), "comment"}, {FourCC("\251too"), "encoder"},
      };
      for (const auto& [type, key] : kKeys) {
        if (item.type == type) {
          media_info_.metadata.emplace_back(
              key, std::string(reinterpret_cast<const char*>(bytes), length));
        }
      }
    }
  }
}

void Mp4Demuxer::SetEnabledStreams(const std::vector<int>& stream_indices) {
  for (Track& track : tracks_) {
    track.enabled = false;
  }
  for (int index : stream_indices) {
    if (index >= 0 && index < static_cast<int>(tracks_.size())) {
      tracks_[index].enabled = true;
    }
  }
}

DemuxerReadResult Mp4Demuxer::Read(EncodedPacket* packet) {
  if (aborted_ || interrupted_) {
    return DemuxerReadResult::kAborted;
  }
  // The enabled track whose next sample decodes first.
  int index = -1;
  for (size_t i = 0; i < tracks_.size(); ++i) {
    const Track& track = tracks_[i];
    if (!track.enabled || track.next >= track.samples.size()) {
      continue;
    }
    if (index < 0 || track.samples[track.next].dts <
                         tracks_[index].samples[tracks_[index].next].dts) {
      index = static_cast<int>(i);
    }
  }
  if (index < 0) {
    return DemuxerReadResult::kEndOfStream;
  }
  Track& track = tracks_[index];
  const Sample& sample = track.samples[track.next];

  class Buffer : public PacketBuffer {
   public:
    explicit Buffer(size_t size) : bytes_(size + kPacketPadding) {
      data_ = bytes_.data();
      size_ = size;
    }
    uint8_t* writable() { return bytes_.data(); }

   private:
    std::vector<uint8_t> bytes_;
  };
  auto fail = [&](const char* reason, const char* detail) {
    char message[160];
    snprintf(message, sizeof(message), reason,
             (sample.pts - media_info_.start_time) / 1e6,
             media_info_.duration / 1e6);
    error_ = message;
    LOGE("%s%s%s", message, *detail ? ": " : "", detail);
    return DemuxerReadResult::kError;
  };
  const char* kEnded =
      "input ended at %.3f s, before its duration of %.3f s "
      "(truncated file or lost connection)";
  if (sample.size > kMaxSampleSize) {
    return fail("sample at %.3f s (of %.3f s) is too large: corrupt file", "");
  }
  // Data past the end of the file: the same as a read that ends early,
  // without allocating for it.
  if (file_size_ > 0 &&
      (sample.offset > static_cast<uint64_t>(file_size_) ||
       sample.size > static_cast<uint64_t>(file_size_) - sample.offset)) {
    return fail(kEnded, "");
  }
  std::shared_ptr<Buffer> buffer;
  try {
    buffer = std::make_shared<Buffer>(sample.size);
  } catch (const std::bad_alloc&) {
    return fail("out of memory for the sample at %.3f s (of %.3f s)", "");
  }
  size_t filled = 0;
  while (filled < sample.size) {
    int64_t n = source_->Read(static_cast<int64_t>(sample.offset + filled),
                              buffer->writable() + filled,
                              sample.size - filled);
    if (n < 0 && (aborted_ || interrupted_ || source_->interrupted())) {
      return DemuxerReadResult::kAborted;
    }
    if (n <= 0) {
      return fail(kEnded, n < 0 ? source_->error().c_str() : "");
    }
    filled += static_cast<size_t>(n);
  }
  packet->stream_index = index;
  packet->pts = sample.pts;
  packet->dts = sample.dts;
  packet->duration = sample.duration;
  packet->keyframe = sample.sync;
  packet->end_of_stream = false;
  packet->boundary = false;
  packet->buffer = std::move(buffer);
  track.next++;
  return DemuxerReadResult::kOk;
}

bool Mp4Demuxer::Seek(int64_t time) {
  interrupted_ = false;
  if (!aborted_) {
    source_->ClearInterrupt();
  }
  // Video lands on the keyframe at or before |time|; the other tracks on
  // the sample that covers that keyframe's time.
  int64_t target = kNoTimestamp;
  for (size_t i = 0; i < tracks_.size(); ++i) {
    if (media_info_.streams[i].type != MediaType::kVideo) {
      continue;
    }
    const std::vector<Sample>& samples = tracks_[i].samples;
    size_t keyframe = samples.size();
    for (size_t j = 0; j < samples.size(); ++j) {
      if (!samples[j].sync) {
        continue;
      }
      if (samples[j].pts <= time || keyframe == samples.size()) {
        keyframe = j;
      }
      if (samples[j].pts > time) {
        break;
      }
    }
    tracks_[i].next = keyframe;
    if (target == kNoTimestamp && tracks_[i].enabled &&
        keyframe < samples.size()) {
      target = samples[keyframe].pts;
    }
  }
  if (target == kNoTimestamp) {
    target = time;
  }
  for (size_t i = 0; i < tracks_.size(); ++i) {
    if (media_info_.streams[i].type == MediaType::kVideo) {
      continue;
    }
    const std::vector<Sample>& samples = tracks_[i].samples;
    auto after = std::upper_bound(
        samples.begin(), samples.end(), target,
        [](int64_t t, const Sample& s) { return t < s.dts; });
    tracks_[i].next = after == samples.begin()
                          ? 0
                          : static_cast<size_t>(after - samples.begin()) - 1;
  }
  return true;
}

void Mp4Demuxer::Interrupt() {
  interrupted_ = true;
  std::lock_guard<std::mutex> lock(source_mutex_);
  if (source_) {
    source_->Interrupt();
  }
}

void Mp4Demuxer::Abort() {
  aborted_ = true;
  std::lock_guard<std::mutex> lock(source_mutex_);
  if (source_) {
    source_->Interrupt();
  }
}

}  // namespace lychee
