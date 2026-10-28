#define LOG_TAG "mp4"

#include "backends/mp4/mp4_demuxer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <optional>

#include "backends/mp4/mp4_subtitle.h"
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
// A moof box lists one fragment's samples: KBs, a few MB for a fragment
// that holds a whole film.
constexpr uint64_t kMaxMoofSize = 32 << 20;
// Samples in one moof (a 2-hour film at 60 fps, with audio, is ~0.8 M),
// and of those that take no bytes of its table (all trex/tfhd defaults).
constexpr size_t kMaxFragmentSamples = 1 << 21;
constexpr size_t kMaxDefaultSamples = 1 << 16;
// mfra and sidx boxes: some bytes per fragment.
constexpr uint64_t kMaxIndexSize = 16 << 20;
// Decode times in ticks and edit offsets are kept below this, so sums of
// a few of them cannot overflow (2^53 ticks at 1 GHz are 104 days).
constexpr int64_t kMaxTicks = int64_t{1} << 53;
// SeekPoint::offset of the moov's own samples.
constexpr int64_t kMoovSamples = -1;

// trun and tfhd flags (ISO/IEC 14496-12 8.8.7, 8.8.8).
constexpr uint32_t kTfhdBaseDataOffset = 0x1;
constexpr uint32_t kTfhdSampleDescription = 0x2;
constexpr uint32_t kTfhdDefaultDuration = 0x8;
constexpr uint32_t kTfhdDefaultSize = 0x10;
constexpr uint32_t kTfhdDefaultFlags = 0x20;
constexpr uint32_t kTfhdDefaultBaseIsMoof = 0x20000;
constexpr uint32_t kTrunDataOffset = 0x1;
constexpr uint32_t kTrunFirstSampleFlags = 0x4;
constexpr uint32_t kTrunDuration = 0x100;
constexpr uint32_t kTrunSize = 0x200;
constexpr uint32_t kTrunFlags = 0x400;
constexpr uint32_t kTrunCompositionOffset = 0x800;
constexpr uint32_t kSampleIsNonSync = 0x10000;

int64_t ClampTicks(int64_t ticks) {
  return std::clamp<int64_t>(ticks, -kMaxTicks, kMaxTicks);
}

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

// |ticks| / |timescale| seconds in microseconds, without overflow: the
// seconds are clamped to 2^33 (272 years, under 2^53 microseconds), and
// |timescale| is a 32-bit field.
int64_t Rescale(int64_t ticks, int64_t timescale) {
  if (timescale <= 0) {
    return 0;
  }
  constexpr int64_t kMaxSeconds = int64_t{1} << 33;
  return std::clamp(ticks / timescale, -kMaxSeconds, kMaxSeconds) *
             kMicrosPerSecond +
         ticks % timescale * kMicrosPerSecond / timescale;
}

// The inverse, for times of up to 24 days (at most kMaxTicks with a
// 32-bit |timescale|).
int64_t ToTicks(int64_t micros, int64_t timescale) {
  constexpr int64_t kMax = (int64_t{1} << 21) * kMicrosPerSecond;
  micros = std::clamp(micros, -kMax, kMax);
  return micros / kMicrosPerSecond * timescale +
         micros % kMicrosPerSecond * timescale / kMicrosPerSecond;
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
    case FourCC("vp09"):
      config->codec = VideoCodec::kVP9;
      config->codec_name = "vp9";
      break;
    case FourCC("av01"):
      config->codec = VideoCodec::kAV1;
      config->codec_name = "av1";
      break;
    default:
      config->codec = VideoCodec::kOther;
      config->codec_name = FourCCName(entry.type);
      break;
  }
  for (const Box& child : Children(reader.current(), reader.remaining())) {
    if (child.type == FourCC("avcC") || child.type == FourCC("hvcC") ||
        child.type == FourCC("av1C")) {
      config->extradata.assign(child.payload,
                               child.payload + child.payload_size);
    } else if (child.type == FourCC("vpcC")) {
      // VP9's system decoders read profile/depth from keyframes. The
      // configuration box also carries the colors, absent from those.
      Reader vpcc(child.payload, child.payload_size);
      if (vpcc.U32() == 0x01000000) {  // version 1, flags 0
        vpcc.Skip(2);                  // profile, level
        const uint8_t flags = vpcc.U8();
        const uint8_t primaries = vpcc.U8();
        const uint8_t transfer = vpcc.U8();
        const uint8_t matrix = vpcc.U8();
        if (vpcc.ok()) {
          config->color = {primaries, transfer, matrix, (flags & 1) != 0};
        }
      }
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
    // Converting a NaN or a huge double to int is undefined.
    config->sample_rate = rate > 0 && rate < 1e7 ? static_cast<int>(rate) : 0;
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
  } else if (entry.type == FourCC("ac-3")) {
    config->codec = AudioCodec::kAC3;
    config->codec_name = "ac3";
    // ETSI TS 102 366, Annex F: dac3 describes the coded channel layout;
    // the sample entry's channel count can be the stereo output layout.
    if (std::optional<Box> dac3 = Find(children, FourCC("dac3"));
        dac3 && dac3->payload_size >= 3) {
      static constexpr int kRates[] = {48000, 44100, 32000};
      static constexpr int kChannels[] = {2, 1, 2, 3, 3, 4, 4, 5};
      const uint8_t* d = dac3->payload;
      if ((d[0] >> 6) < 3) {
        config->sample_rate = kRates[d[0] >> 6];
      }
      config->channels = kChannels[(d[1] >> 3) & 7] + ((d[1] >> 2) & 1);
    }
  } else if (entry.type == FourCC("alac")) {
    config->codec = AudioCodec::kALAC;
    config->codec_name = "alac";
    // The whole inner alac box, header included, as FFmpeg's extradata
    // and AudioToolbox's cookie have it.
    if (std::optional<Box> alac = Find(children, FourCC("alac"))) {
      config->extradata.assign(alac->start, alac->start + alac->size);
    }
  } else if (entry.type == FourCC("Opus")) {
    config->codec = AudioCodec::kOpus;
    config->codec_name = "opus";
    // dOps ("Encapsulation of Opus in ISO Base Media File Format") holds
    // the fields of the OpusHead packet (RFC 7845) big-endian; FFmpeg's
    // extradata and MediaCodec's csd-0 are the packet itself.
    std::optional<Box> dops = Find(children, FourCC("dOps"));
    if (dops && dops->payload_size >= 11) {
      const uint8_t* d = dops->payload;
      config->extradata = {'O',  'p',  'u',  's',  'H',  'e',  'a',
                           'd',  1,    d[1], d[3], d[2], d[7], d[6],
                           d[5], d[4], d[9], d[8], d[10]};
      // The channel mapping table, when the family is not 0.
      config->extradata.insert(config->extradata.end(), d + 11,
                               d + dops->payload_size);
    }
  } else if (entry.type == FourCC("fLaC")) {
    config->codec = AudioCodec::kFLAC;
    config->codec_name = "flac";
    // dfLa holds FLAC metadata blocks, STREAMINFO first; FFmpeg's
    // extradata is that block without its 4-byte header.
    std::optional<Box> dfla = Find(children, FourCC("dfLa"));
    constexpr size_t kStreamInfoSize = 34;
    if (dfla && dfla->payload_size >= 8 + kStreamInfoSize &&
        (dfla->payload[4] & 0x7f) == 0) {
      config->extradata.assign(dfla->payload + 8,
                               dfla->payload + 8 + kStreamInfoSize);
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
  return Open(std::move(source), error);
}

bool Mp4Demuxer::Open(std::unique_ptr<DataSource> source, std::string* error) {
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
    if (!reader.ok() || size < header_size ||
        size > static_cast<uint64_t>(INT64_MAX - offset)) {
      *error = "malformed MP4 box";
      return false;
    }
    if (type == FourCC("moof")) {
      *error = "moof box before the moov box";
      return false;
    }
    if (type == FourCC("moov")) {
      if (size > kMaxMoovSize) {
        *error = "moov box too large";
        return false;
      }
      if (file_size > 0 && size > static_cast<uint64_t>(file_size - offset)) {
        *error = "truncated moov box";
        return false;
      }
      file_size_ = file_size;
      try {
        std::vector<uint8_t> moov(size - header_size);
        LoadResult result =
            ReadFully(offset + static_cast<int64_t>(header_size), moov.data(),
                      moov.size());
        if (result != LoadResult::kLoaded) {
          *error = result == LoadResult::kEnd ? "truncated moov box" : error_;
          return false;
        }
        if (!ParseMoov(moov.data(), moov.size(), error)) {
          return false;
        }
        moov.clear();
        moov.shrink_to_fit();
        if (fragmented_) {
          first_fragment_offset_ = offset + static_cast<int64_t>(size);
          if (!OpenFragments(error)) {
            return false;
          }
        }
      } catch (const std::bad_alloc&) {
        *error = "out of memory for the sample tables";
        return false;
      }
      int64_t start = kInfiniteTime;
      for (const Track& track : tracks_) {
        if (!track.samples.empty()) {
          start = std::min(start, track.samples.front().pts);
        }
      }
      // Audio priming (an edit list that skips the encoder delay) starts
      // before zero; the presentation does not.
      media_info_.start_time =
          start == kInfiniteTime ? 0 : std::max<int64_t>(start, 0);
      media_info_.seekable = file_size > 0;
      return true;
    }
    offset += static_cast<int64_t>(size);
  }
}

Mp4Demuxer::LoadResult Mp4Demuxer::ReadFully(int64_t offset,
                                             uint8_t* data,
                                             size_t size) {
  size_t filled = 0;
  while (filled < size) {
    int64_t n = source_->Read(offset + static_cast<int64_t>(filled),
                              data + filled, size - filled);
    if (n < 0) {
      if (aborted() || source_->interrupted()) {
        return LoadResult::kAborted;
      }
      error_ = source_->error();
      return LoadResult::kError;
    }
    if (n == 0) {
      return LoadResult::kEnd;
    }
    filled += static_cast<size_t>(n);
  }
  return LoadResult::kLoaded;
}

bool Mp4Demuxer::ParseMoov(const uint8_t* data, size_t size, std::string* error) {
  std::vector<Box> boxes = Children(data, size);
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
  std::optional<Box> mvex = Find(boxes, FourCC("mvex"));
  fragmented_ = mvex.has_value();
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
    *error = "no supported track";
    return false;
  }
  if (fragmented_) {
    // mvhd covers only the moov's own samples.
    for (const Box& box : Children(mvex->payload, mvex->payload_size)) {
      Reader reader(box.payload, box.payload_size);
      int box_version = reader.U8();
      reader.Skip(3);
      if (box.type == FourCC("mehd")) {
        auto fragment_duration = static_cast<int64_t>(
            box_version == 1 ? reader.U64() : reader.U32());
        if (reader.ok() && fragment_duration > 0) {
          fragment_duration_ = Rescale(fragment_duration, timescale);
        }
      } else if (box.type == FourCC("trex")) {
        uint32_t id = reader.U32();
        reader.Skip(4);  // default_sample_description_index
        uint32_t default_duration = reader.U32();
        uint32_t default_size = reader.U32();
        uint32_t default_flags = reader.U32();
        for (Track& track : tracks_) {
          if (reader.ok() && track.id == id) {
            track.default_duration = default_duration;
            track.default_size = default_size;
            track.default_flags = default_flags;
          }
        }
      }
    }
    return true;
  }
  if (duration > 0 && duration != (version == 1 ? -1 : 0xffffffff)) {
    media_info_.duration = Rescale(duration, timescale);
  } else {
    for (const StreamInfo& stream : media_info_.streams) {
      media_info_.duration = std::max(media_info_.duration, stream.duration);
    }
  }
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
  } else if (handler_type == FourCC("sbtl") || handler_type == FourCC("text") ||
             handler_type == FourCC("subt")) {
    info.type = MediaType::kSubtitle;
  } else {
    return false;
  }

  Reader track_header(tkhd->payload, tkhd->payload_size);
  int version = track_header.U8();
  uint32_t flags = static_cast<uint32_t>(track_header.Bits(3));
  track_header.Skip(version == 1 ? 16 : 8);  // creation, modification
  uint32_t track_id = track_header.U32();
  // reserved, duration, reserved, layer, alternate_group, volume, reserved.
  track_header.Skip(version == 1 ? 12 : 8);
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
  } else if (info.type == MediaType::kAudio) {
    ParseAudioEntry(entries[0], &info.audio);
  } else {
    const uint32_t type = entries[0].type;
    if (type == FourCC("wvtt") || type == FourCC("stpp")) {
      // Unpack the sample before HLS shifts its track timestamps. Both
      // builds then consume ordinary WebVTT cues through their decoder.
      info.subtitle.codec_name = "webvtt";
    } else if (type == FourCC("tx3g") || type == FourCC("text")) {
      info.subtitle.codec_name = "mov_text";
      if (entries[0].payload_size > 8) {
        info.subtitle.extradata.assign(
            entries[0].payload + 8,
            entries[0].payload + entries[0].payload_size);
      }
    } else {
      return false;
    }
    info.subtitle.text = true;
  }

  Track track;
  track.type = info.type;
  if (info.type == MediaType::kSubtitle &&
      info.subtitle.codec_name == "webvtt") {
    track.subtitle_format = entries[0].type == FourCC("wvtt") ? "wvtt" : "stpp";
  }
  track.id = track_id;
  track.timescale = timescale;
  // Edit list: leading empty edits delay the track; the first media edit
  // says where in the media it starts.
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
        if (!edits.ok()) {
          break;
        }
        if (time == -1) {
          track.empty =
              ClampTicks(track.empty + Rescale(segment, movie_timescale));
        } else {
          track.media_time = std::clamp<int64_t>(time, 0, kMaxTicks);
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
    // Field sizes are 4, 8 or 16 bits. Another (0 reads no bytes, so the
    // reader never runs out) would let the count alone fill memory.
    if ((field != 4 && field != 8 && field != 16) ||
        uint64_t{count} * field > uint64_t{stz2.remaining()} * 8) {
      LOGW("track %zu: bad stz2 (%d-bit, %u samples); skipped", tracks_.size(),
           field, count);
      return false;
    }
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
  track.moov_decode_time = ClampTicks(dts);
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
  for (int32_t offset : composition) {
    track.dts_shift = std::max<int64_t>(track.dts_shift, -int64_t{offset});
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
    s = MakeSample(track, s.offset, s.size, all_sync || s.sync, s.dts,
                   composition[i], s.duration);
  }
  // Fragmented files may keep all their samples in the fragments.
  if (track.samples.empty() && !fragmented_) {
    return false;
  }

  info.index = static_cast<int>(media_info_.streams.size());
  info.duration = Rescale(media_duration, timescale);
  if (info.type == MediaType::kVideo && media_info_.default_video_stream < 0) {
    media_info_.default_video_stream = info.index;
  } else if (info.type == MediaType::kAudio &&
             media_info_.default_audio_stream < 0) {
    media_info_.default_audio_stream = info.index;
  } else if (info.type == MediaType::kSubtitle && info.default_track &&
             media_info_.default_subtitle_stream < 0) {
    // FFmpeg reads tkhd's enabled flag as the default disposition.
    media_info_.default_subtitle_stream = info.index;
  }
  media_info_.streams.push_back(std::move(info));
  tracks_.push_back(std::move(track));
  return true;
}

Mp4Demuxer::Sample Mp4Demuxer::MakeSample(const Track& track,
                                          uint64_t offset,
                                          uint32_t size,
                                          bool sync,
                                          int64_t dts,
                                          int64_t composition,
                                          int64_t duration) const {
  Sample sample{};
  sample.offset = offset;
  sample.size = size;
  sample.sync = sync;
  sample.pts = Rescale(dts + composition - track.media_time, track.timescale) +
               track.empty;
  sample.dts =
      Rescale(dts - track.dts_shift - track.media_time, track.timescale) +
      track.empty;
  sample.duration = Rescale(duration, track.timescale);
  return sample;
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

bool Mp4Demuxer::OpenFragments(std::string* error) {
  seek_track_ = std::max(media_info_.default_video_stream, 0);
  int64_t moov_end = kNoTimestamp;
  for (Track& track : tracks_) {
    track.moov_samples = track.samples;
    track.decode_time = track.moov_decode_time;
    scan_decode_times_.push_back(track.moov_decode_time);
    for (const Sample& sample : track.samples) {
      moov_end = std::max(moov_end, sample.pts + sample.duration);
    }
  }
  scan_offset_ = first_fragment_offset_;
  if (file_size_ > 0 && !ReadMfra()) {
    ReadSidx();
  }
  if (aborted()) {
    *error = "aborted";
    return false;
  }
  const bool moov_samples = !tracks_[seek_track_].moov_samples.empty();
  if (moov_samples) {
    const std::vector<Sample>& samples = tracks_[seek_track_].moov_samples;
    auto sync = std::find_if(samples.begin(), samples.end(),
                             [](const Sample& s) { return s.sync; });
    seek_points_.insert(seek_points_.begin(),
                        {sync == samples.end() ? 0 : sync->pts, kMoovSamples});
  }
  LoadResult result =
      StartAt(moov_samples ? kMoovSamples : first_fragment_offset_, 0);
  if (result == LoadResult::kLoaded) {
    // Each track's first sample, for the start time: with a moof per
    // track, the first holds only one.
    result = FillFragments();
  }
  if (result == LoadResult::kLoaded && fragment_duration_ == kNoTimestamp &&
      file_size_ > 0) {
    // The duration is where the last fragment ends: from the last one the
    // index knows, or by reading them all.
    if (index_complete_) {
      int64_t offset = first_fragment_offset_;
      std::vector<int64_t> decode_times = scan_decode_times_;
      if (!seek_points_.empty() && seek_points_.back().offset != kMoovSamples) {
        offset = seek_points_.back().offset;
        decode_times = DecodeTimesAt(seek_points_.back().time);
      }
      result = Scan(&offset, &decode_times, kInfiniteTime, false);
    } else {
      result = ScanIndex(kInfiniteTime);
    }
  }
  if (result == LoadResult::kAborted || result == LoadResult::kError) {
    *error = result == LoadResult::kAborted ? "aborted" : error_;
    return false;
  }
  if (fragment_duration_ != kNoTimestamp) {
    media_info_.duration = fragment_duration_;
  } else if (fragment_end_ != kNoTimestamp || moov_end != kNoTimestamp) {
    media_info_.duration = std::max(fragment_end_, moov_end);
  }
  LOGI(
      "fragmented: %zu seek points%s, duration %.3f s", seek_points_.size(),
      index_complete_ ? "" : " so far",
      media_info_.duration == kNoTimestamp ? -1.0 : media_info_.duration / 1e6);
  return true;
}

bool Mp4Demuxer::ReadMfra() {
  // mfro, the last box, gives the size of mfra, which it ends.
  uint8_t tail[16];
  if (file_size_ < 16 ||
      ReadFully(file_size_ - 16, tail, sizeof(tail)) != LoadResult::kLoaded) {
    return false;
  }
  Reader mfro(tail, sizeof(tail));
  uint32_t mfro_size = mfro.U32();
  uint32_t mfro_type = mfro.U32();
  mfro.Skip(4);
  uint64_t size = mfro.U32();
  if (mfro_size != 16 || mfro_type != FourCC("mfro") || size < 16 ||
      size > kMaxIndexSize || size > static_cast<uint64_t>(file_size_)) {
    return false;
  }
  std::vector<uint8_t> mfra(size);
  const int64_t offset = file_size_ - static_cast<int64_t>(size);
  if (ReadFully(offset, mfra.data(), mfra.size()) != LoadResult::kLoaded) {
    return false;
  }
  std::vector<Box> boxes = Children(mfra.data(), mfra.size());
  if (boxes.size() != 1 || boxes[0].type != FourCC("mfra")) {
    return false;
  }
  const Track& track = tracks_[seek_track_];
  std::vector<SeekPoint> points;
  for (const Box& tfra : Children(boxes[0].payload, boxes[0].payload_size)) {
    if (tfra.type != FourCC("tfra")) {
      continue;
    }
    Reader reader(tfra.payload, tfra.payload_size);
    int version = reader.U8();
    reader.Skip(3);
    if (reader.U32() != track.id) {
      continue;
    }
    uint32_t lengths = reader.U32();
    uint32_t count = reader.U32();
    // traf_number, trun_number and sample_number take 1 to 4 bytes each.
    const size_t numbers =
        ((lengths >> 4) & 3) + ((lengths >> 2) & 3) + (lengths & 3) + 3;
    for (uint32_t i = 0; i < count && reader.ok(); ++i) {
      int64_t time =
          static_cast<int64_t>(version == 1 ? reader.U64() : reader.U32());
      auto moof =
          static_cast<int64_t>(version == 1 ? reader.U64() : reader.U32());
      reader.Skip(numbers);
      if (reader.ok() && moof >= first_fragment_offset_ && moof < offset) {
        points.push_back(
            {Rescale(ClampTicks(time) - track.media_time, track.timescale) +
                 track.empty,
             moof});
      }
    }
  }
  if (points.empty()) {
    return false;
  }
  std::stable_sort(
      points.begin(), points.end(),
      [](const SeekPoint& a, const SeekPoint& b) { return a.time < b.time; });
  seek_points_ = std::move(points);
  index_complete_ = true;
  return true;
}

void Mp4Demuxer::ReadSidx() {
  // The boxes between the moov and the first moof. A sidx there indexes
  // the whole file (global_sidx) or, in DASH segments, only the next one;
  // only the first kind is an index.
  int64_t offset = first_fragment_offset_;
  for (int boxes = 0; boxes < 16; ++boxes) {
    uint32_t type;
    uint64_t size;
    uint64_t header;
    if (ReadBoxHeader(offset, &type, &size, &header) != LoadResult::kLoaded ||
        type == FourCC("moof") || type == FourCC("mdat")) {
      return;
    }
    const int64_t box_offset = offset;
    offset += static_cast<int64_t>(size);
    if (type != FourCC("sidx") || size > kMaxIndexSize) {
      continue;
    }
    std::vector<uint8_t> sidx(size - header);
    if (ReadFully(box_offset + static_cast<int64_t>(header), sidx.data(),
                  sidx.size()) != LoadResult::kLoaded) {
      return;
    }
    Reader reader(sidx.data(), sidx.size());
    int version = reader.U8();
    reader.Skip(3);
    uint32_t id = reader.U32();
    int64_t timescale = reader.U32();
    auto time =
        static_cast<int64_t>(version == 1 ? reader.U64() : reader.U32());
    uint64_t first_offset = version == 1 ? reader.U64() : reader.U32();
    reader.Skip(2);
    uint16_t count = reader.U16();
    // Another track's index (one per track) points at the same moofs, but
    // its times are in its own timescale.
    auto track = std::find_if(tracks_.begin(), tracks_.end(),
                              [id](const Track& t) { return t.id == id; });
    if (!reader.ok() || timescale <= 0 || track == tracks_.end() ||
        track - tracks_.begin() != seek_track_ ||
        first_offset > static_cast<uint64_t>(file_size_)) {
      continue;
    }
    std::vector<SeekPoint> points;
    int64_t moof = offset + static_cast<int64_t>(first_offset);
    time = ClampTicks(time);
    for (uint16_t i = 0; i < count; ++i) {
      uint32_t reference = reader.U32();
      uint32_t duration = reader.U32();
      reader.Skip(4);  // SAP
      if (!reader.ok() || (reference & 0x80000000) || moof >= file_size_) {
        // A sidx of sidx boxes is not followed.
        return;
      }
      points.push_back({Rescale(time, timescale) -
                            Rescale(track->media_time, track->timescale) +
                            track->empty,
                        moof});
      moof += reference;
      time = ClampTicks(time + duration);
    }
    if (moof != file_size_ || points.empty()) {
      return;
    }
    seek_points_ = std::move(points);
    index_complete_ = true;
    return;
  }
}

Mp4Demuxer::LoadResult Mp4Demuxer::ReadBoxHeader(int64_t offset,
                                                 uint32_t* type,
                                                 uint64_t* size,
                                                 uint64_t* header) {
  if (offset < 0 || (file_size_ > 0 && offset >= file_size_)) {
    return LoadResult::kEnd;
  }
  uint8_t bytes[16];
  int64_t got = source_->Read(offset, bytes, sizeof(bytes));
  if (got < 0) {
    if (aborted() || source_->interrupted()) {
      return LoadResult::kAborted;
    }
    error_ = source_->error();
    return LoadResult::kError;
  }
  Reader reader(bytes, static_cast<size_t>(got));
  *size = reader.U32();
  *type = reader.U32();
  *header = 8;
  if (*size == 1) {
    *size = reader.U64();
    *header = 16;
  } else if (*size == 0) {
    // To the end of the data.
    if (file_size_ <= 0) {
      return LoadResult::kEnd;
    }
    *size = static_cast<uint64_t>(file_size_ - offset);
  }
  if (!reader.ok()) {
    return LoadResult::kEnd;
  }
  if (*size < *header || *size > static_cast<uint64_t>(INT64_MAX - offset)) {
    error_ = "malformed MP4 box";
    return LoadResult::kError;
  }
  return LoadResult::kLoaded;
}

Mp4Demuxer::LoadResult Mp4Demuxer::ReadMoof(int64_t offset,
                                            uint64_t size,
                                            std::vector<uint8_t>* moof) {
  if (size > kMaxMoofSize) {
    error_ = "moof box too large: corrupt file";
    return LoadResult::kError;
  }
  if (file_size_ > 0 && size > static_cast<uint64_t>(file_size_ - offset)) {
    return LoadResult::kEnd;
  }
  moof->resize(size);
  return ReadFully(offset, moof->data(), moof->size());
}

void Mp4Demuxer::ParseMoof(const uint8_t* data,
                           size_t size,
                           int64_t offset,
                           bool store,
                           std::vector<int64_t>* decode_times,
                           MoofSummary* summary) {
  std::vector<Box> boxes = Children(data, size);
  if (boxes.empty()) {
    return;
  }
  const Box& moof = boxes[0];
  size_t budget = kMaxFragmentSamples;
  // Where the previous traf's data ended: the next one's base, unless it
  // says otherwise.
  uint64_t previous_end = static_cast<uint64_t>(offset);
  bool first_traf = true;
  for (const Box& traf : Children(moof.payload, moof.payload_size)) {
    if (traf.type != FourCC("traf")) {
      continue;
    }
    std::vector<Box> children = Children(traf.payload, traf.payload_size);
    std::optional<Box> tfhd = Find(children, FourCC("tfhd"));
    if (!tfhd) {
      continue;
    }
    Reader header(tfhd->payload, tfhd->payload_size);
    header.Skip(1);
    auto flags = static_cast<uint32_t>(header.Bits(3));
    uint32_t id = header.U32();
    auto track_it = std::find_if(tracks_.begin(), tracks_.end(),
                                 [id](const Track& t) { return t.id == id; });
    if (!header.ok() || track_it == tracks_.end()) {
      continue;
    }
    const size_t index = static_cast<size_t>(track_it - tracks_.begin());
    Track& track = *track_it;
    uint64_t base = first_traf || (flags & kTfhdDefaultBaseIsMoof)
                        ? static_cast<uint64_t>(offset)
                        : previous_end;
    if (flags & kTfhdBaseDataOffset) {
      base = header.U64();
    }
    if (flags & kTfhdSampleDescription) {
      header.Skip(4);
    }
    uint32_t default_duration =
        flags & kTfhdDefaultDuration ? header.U32() : track.default_duration;
    uint32_t default_size =
        flags & kTfhdDefaultSize ? header.U32() : track.default_size;
    uint32_t default_flags =
        flags & kTfhdDefaultFlags ? header.U32() : track.default_flags;
    if (!header.ok()) {
      continue;
    }
    first_traf = false;
    int64_t dts = (*decode_times)[index];
    if (std::optional<Box> tfdt = Find(children, FourCC("tfdt"))) {
      Reader time(tfdt->payload, tfdt->payload_size);
      int version = time.U8();
      time.Skip(3);
      uint64_t decode_time = version == 1 ? time.U64() : time.U32();
      if (time.ok()) {
        dts = static_cast<int64_t>(std::min<uint64_t>(decode_time, kMaxTicks));
      }
    }
    uint64_t position = base;
    for (const Box& trun : children) {
      if (trun.type != FourCC("trun")) {
        continue;
      }
      Reader run(trun.payload, trun.payload_size);
      run.Skip(1);
      auto run_flags = static_cast<uint32_t>(run.Bits(3));
      size_t count = run.U32();
      if (run_flags & kTrunDataOffset) {
        position = base + static_cast<uint64_t>(static_cast<int64_t>(
                              static_cast<int32_t>(run.U32())));
      }
      uint32_t first_flags = run_flags & kTrunFirstSampleFlags ? run.U32() : 0;
      if (!run.ok()) {
        break;
      }
      // Bound the count by the table, or without one by the bytes the
      // samples claim.
      const size_t entry =
          size_t{4} *
          (((run_flags & kTrunDuration) != 0) + ((run_flags & kTrunSize) != 0) +
           ((run_flags & kTrunFlags) != 0) +
           ((run_flags & kTrunCompositionOffset) != 0));
      if (entry > 0) {
        count = std::min(count, run.remaining() / entry);
      } else {
        count = std::min(count, kMaxDefaultSamples);
        if (default_size > 0 && file_size_ > 0) {
          count = std::min<size_t>(
              count, static_cast<uint64_t>(file_size_) / default_size);
        }
      }
      if (count > budget) {
        LOGW("moof at %lld: more than %zu samples; the rest skipped",
             static_cast<long long>(offset), kMaxFragmentSamples);
        count = budget;
      }
      budget -= count;
      if (store && (run_flags & kTrunCompositionOffset)) {
        // The offsets first, for dts_shift.
        Reader offsets = run;
        const size_t before = entry - 4;
        for (size_t i = 0; i < count && offsets.ok(); ++i) {
          offsets.Skip(before);
          track.dts_shift = std::max<int64_t>(
              track.dts_shift, -int64_t{static_cast<int32_t>(offsets.U32())});
        }
      }
      for (size_t i = 0; i < count; ++i) {
        uint32_t duration =
            run_flags & kTrunDuration ? run.U32() : default_duration;
        uint32_t sample_size = run_flags & kTrunSize ? run.U32() : default_size;
        uint32_t sample_flags =
            run_flags & kTrunFlags ? run.U32() : default_flags;
        if (i == 0 && (run_flags & kTrunFirstSampleFlags)) {
          sample_flags = first_flags;
        }
        int64_t composition = run_flags & kTrunCompositionOffset
                                  ? static_cast<int32_t>(run.U32())
                                  : 0;
        if (!run.ok()) {
          break;
        }
        const bool sync = track.type != MediaType::kVideo ||
                          !(sample_flags & kSampleIsNonSync);
        Sample sample = MakeSample(track, position, sample_size, sync, dts,
                                   composition, duration);
        summary->samples++;
        if (summary->start == kNoTimestamp || sample.dts < summary->start) {
          summary->start = sample.dts;
        }
        summary->end = std::max(summary->end, sample.pts + sample.duration);
        if (sync && index == static_cast<size_t>(seek_track_) &&
            summary->sync_time == kNoTimestamp) {
          summary->sync_time = sample.pts;
        }
        if (store && track.enabled) {
          track.samples.push_back(sample);
        }
        position += sample_size;
        dts = ClampTicks(dts + duration);
      }
    }
    previous_end = position;
    (*decode_times)[index] = dts;
  }
}

Mp4Demuxer::LoadResult Mp4Demuxer::LoadNextFragment() {
  // Only what was not read yet stays.
  for (Track& track : tracks_) {
    if (track.enabled) {
      track.samples.erase(
          track.samples.begin(),
          track.samples.begin() + static_cast<ptrdiff_t>(track.next));
    } else {
      track.samples.clear();
    }
    track.next = 0;
  }
  for (;;) {
    if (aborted()) {
      return LoadResult::kAborted;
    }
    uint32_t type;
    uint64_t size;
    uint64_t header;
    const int64_t offset = next_fragment_offset_;
    LoadResult result = ReadBoxHeader(offset, &type, &size, &header);
    if (result != LoadResult::kLoaded || type == FourCC("mfra")) {
      if (result != LoadResult::kAborted && result != LoadResult::kError) {
        fragments_ended_ = true;
        return LoadResult::kEnd;
      }
      return result;
    }
    next_fragment_offset_ = offset + static_cast<int64_t>(size);
    if (type != FourCC("moof")) {
      continue;  // mdat, sidx, styp, free, ...
    }
    std::vector<uint8_t> moof;
    result = ReadMoof(offset, size, &moof);
    if (result == LoadResult::kEnd) {
      error_ = "input ended in a moof box (truncated file or lost connection)";
      return LoadResult::kError;
    }
    if (result != LoadResult::kLoaded) {
      return result;
    }
    std::vector<int64_t> decode_times;
    decode_times.reserve(tracks_.size());
    for (const Track& track : tracks_) {
      decode_times.push_back(track.decode_time);
    }
    MoofSummary summary;
    ParseMoof(moof.data(), moof.size(), offset, true, &decode_times, &summary);
    for (size_t i = 0; i < tracks_.size(); ++i) {
      tracks_[i].decode_time = decode_times[i];
    }
    last_fragment_start_ = summary.start;
    return LoadResult::kLoaded;
  }
}

Mp4Demuxer::LoadResult Mp4Demuxer::FillFragments() {
  for (;;) {
    int64_t candidate = kInfiniteTime;
    bool starving = false;
    for (Track& track : tracks_) {
      if (!track.enabled) {
        continue;
      }
      // After a seek: from the sample that covers the target (for video,
      // from a keyframe).
      while (track.skip_before != kNoTimestamp &&
             track.next < track.samples.size()) {
        const Sample& sample = track.samples[track.next];
        if (sample.dts + sample.duration > track.skip_before &&
            (sample.sync || track.type != MediaType::kVideo)) {
          track.skip_before = kNoTimestamp;
          break;
        }
        track.next++;
      }
      if (track.next < track.samples.size()) {
        candidate = std::min(candidate, track.samples[track.next].dts);
      } else {
        starving = true;
      }
    }
    // A track without a sample loaded may have one in the next fragment
    // that comes first; not once the fragments loaded start later.
    if (!starving || fragments_ended_ ||
        (candidate != kInfiniteTime && last_fragment_start_ != kNoTimestamp &&
         last_fragment_start_ > candidate)) {
      return LoadResult::kLoaded;
    }
    LoadResult result = LoadNextFragment();
    if (result == LoadResult::kError || result == LoadResult::kAborted) {
      return result;
    }
  }
}

Mp4Demuxer::LoadResult Mp4Demuxer::StartAt(int64_t offset, int64_t time) {
  fragments_ended_ = false;
  last_fragment_start_ = kNoTimestamp;
  std::vector<int64_t> decode_times = DecodeTimesAt(time);
  for (size_t i = 0; i < tracks_.size(); ++i) {
    Track& track = tracks_[i];
    track.samples.clear();
    track.next = 0;
    track.skip_before = kNoTimestamp;
    track.decode_time = decode_times[i];
    if (offset == kMoovSamples) {
      if (track.enabled) {
        track.samples = track.moov_samples;
      }
      track.decode_time = track.moov_decode_time;
    }
  }
  if (offset == kMoovSamples) {
    next_fragment_offset_ = first_fragment_offset_;
    return LoadResult::kLoaded;
  }
  next_fragment_offset_ = offset;
  return LoadNextFragment();
}

std::vector<int64_t> Mp4Demuxer::DecodeTimesAt(int64_t time) const {
  std::vector<int64_t> decode_times;
  decode_times.reserve(tracks_.size());
  for (const Track& track : tracks_) {
    decode_times.push_back(ClampTicks(
        ToTicks(time - track.empty, track.timescale) + track.media_time));
  }
  return decode_times;
}

Mp4Demuxer::LoadResult Mp4Demuxer::Scan(int64_t* offset,
                                        std::vector<int64_t>* decode_times,
                                        int64_t until,
                                        bool add_points) {
  for (;;) {
    if (aborted()) {
      return LoadResult::kAborted;
    }
    uint32_t type;
    uint64_t size;
    uint64_t header;
    const int64_t box_offset = *offset;
    LoadResult result = ReadBoxHeader(box_offset, &type, &size, &header);
    if (result != LoadResult::kLoaded || type == FourCC("mfra")) {
      return result == LoadResult::kLoaded ? LoadResult::kEnd : result;
    }
    *offset = box_offset + static_cast<int64_t>(size);
    if (type != FourCC("moof")) {
      continue;
    }
    std::vector<uint8_t> moof;
    result = ReadMoof(box_offset, size, &moof);
    if (result != LoadResult::kLoaded) {
      // A cut moof ends the index; reading it fails later.
      return result;
    }
    MoofSummary summary;
    ParseMoof(moof.data(), moof.size(), box_offset, false, decode_times,
              &summary);
    fragment_end_ = std::max(fragment_end_, summary.end);
    scanned_samples_ += summary.samples;
    if (scanned_samples_ > kMaxSamples) {
      LOGW("more than %zu samples in the fragments; the rest not indexed",
           kMaxSamples);
      return LoadResult::kEnd;
    }
    if (add_points && summary.sync_time != kNoTimestamp) {
      SeekPoint point = {summary.sync_time, box_offset};
      auto at = std::upper_bound(
          seek_points_.begin(), seek_points_.end(), point.time,
          [](int64_t t, const SeekPoint& p) { return t < p.time; });
      seek_points_.insert(at, point);
      if (point.time > until) {
        return LoadResult::kLoaded;
      }
    }
  }
}

Mp4Demuxer::LoadResult Mp4Demuxer::ScanIndex(int64_t time) {
  if (index_complete_ ||
      (!seek_points_.empty() &&
       seek_points_.back().offset >= first_fragment_offset_ &&
       seek_points_.back().time > time)) {
    return LoadResult::kLoaded;
  }
  LoadResult result = Scan(&scan_offset_, &scan_decode_times_, time, true);
  if (result == LoadResult::kEnd) {
    index_complete_ = true;
    return LoadResult::kLoaded;
  }
  return result;
}

bool Mp4Demuxer::SeekFragments(int64_t time) {
  if (file_size_ > 0) {
    LoadResult result = ScanIndex(time);
    if (result != LoadResult::kLoaded) {
      LOGW("seek to %.3f s: %s", time / 1e6,
           result == LoadResult::kAborted ? "interrupted" : error_.c_str());
      return false;
    }
  }
  // The last seek point at or before |time|. Its fragment may hold no
  // keyframe at or before it (an index of fragment starts, B-frames):
  // then the one before.
  size_t point = static_cast<size_t>(
      std::upper_bound(
          seek_points_.begin(), seek_points_.end(), time,
          [](int64_t t, const SeekPoint& p) { return t < p.time; }) -
      seek_points_.begin());
  point = point > 0 ? point - 1 : 0;
  Track& seek_track = tracks_[seek_track_];
  size_t keyframe;
  for (;;) {
    LoadResult result =
        seek_points_.empty()
            ? StartAt(first_fragment_offset_, 0)
            : StartAt(seek_points_[point].offset, seek_points_[point].time);
    if (result == LoadResult::kError || result == LoadResult::kAborted) {
      LOGW("seek to %.3f s: %s", time / 1e6,
           result == LoadResult::kAborted ? "interrupted" : error_.c_str());
      return false;
    }
    const std::vector<Sample>& samples = seek_track.samples;
    keyframe = samples.size();
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
    const bool before =
        keyframe < samples.size() && samples[keyframe].pts <= time;
    if (before || point == 0 || !seek_track.enabled) {
      break;
    }
    --point;
  }
  int64_t target = time;
  if (keyframe < seek_track.samples.size()) {
    seek_track.next = keyframe;
    target = seek_track.samples[keyframe].pts;
  } else {
    seek_track.skip_before = time;
  }
  for (size_t i = 0; i < tracks_.size(); ++i) {
    if (i != static_cast<size_t>(seek_track_)) {
      tracks_[i].skip_before = target;
    }
  }
  return true;
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
  std::erase_if(subtitle_packets_, [this](const EncodedPacket& packet) {
    return !tracks_[packet.stream_index].enabled;
  });
}

DemuxerReadResult Mp4Demuxer::Fail(int64_t at,
                                   const char* reason,
                                   const char* detail) {
  char message[160];
  snprintf(
      message, sizeof(message), reason, (at - media_info_.start_time) / 1e6,
      media_info_.duration == kNoTimestamp ? 0.0 : media_info_.duration / 1e6);
  error_ = message;
  LOGE("%s%s%s", message, *detail ? ": " : "", detail);
  return DemuxerReadResult::kError;
}

DemuxerReadResult Mp4Demuxer::Read(EncodedPacket* packet) {
  if (aborted()) {
    return DemuxerReadResult::kAborted;
  }
  for (;;) {
    if (fragmented_) {
      LoadResult result = FillFragments();
      if (result == LoadResult::kAborted) {
        return DemuxerReadResult::kAborted;
      }
      if (result == LoadResult::kError) {
        LOGE("%s", error_.c_str());
        return DemuxerReadResult::kError;
      }
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
    if (!subtitle_packets_.empty() &&
        (index < 0 || subtitle_packets_.front().dts <=
                          tracks_[index].samples[tracks_[index].next].dts)) {
      *packet = std::move(subtitle_packets_.front());
      subtitle_packets_.pop_front();
      return DemuxerReadResult::kOk;
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
    const char* kEnded =
        "input ended at %.3f s, before its duration of %.3f s "
        "(truncated file or lost connection)";
    if (sample.size > kMaxSampleSize) {
      return Fail(sample.pts,
                  "sample at %.3f s (of %.3f s) is too large: corrupt file",
                  "");
    }
    // Data past the end of the file: the same as a read that ends early,
    // without allocating for it.
    if (file_size_ > 0 &&
        (sample.offset > static_cast<uint64_t>(file_size_) ||
         sample.size > static_cast<uint64_t>(file_size_) - sample.offset)) {
      return Fail(sample.pts, kEnded, "");
    }
    std::shared_ptr<Buffer> buffer;
    try {
      buffer = std::make_shared<Buffer>(sample.size);
    } catch (const std::bad_alloc&) {
      return Fail(sample.pts,
                  "out of memory for the sample at %.3f s (of %.3f s)", "");
    }
    size_t filled = 0;
    while (filled < sample.size) {
      int64_t n =
          source_->Read(static_cast<int64_t>(sample.offset + filled),
                        buffer->writable() + filled, sample.size - filled);
      if (n < 0 && (aborted() || source_->interrupted())) {
        return DemuxerReadResult::kAborted;
      }
      if (n <= 0) {
        return Fail(sample.pts, kEnded, n < 0 ? source_->error().c_str() : "");
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
    if (!track.subtitle_format.empty()) {
      std::vector<SubtitleCue> cues;
      const int64_t subtitle_offset =
          track.empty - Rescale(track.media_time, track.timescale);
      if (!ParseMp4Subtitle(
              track.subtitle_format,
              std::string_view(
                  reinterpret_cast<const char*>(packet->buffer->data()),
                  packet->buffer->size()),
              packet->pts - subtitle_offset, packet->duration, &cues)) {
        return Fail(packet->pts,
                    "invalid subtitle sample at %.3f s (of %.3f s)", "");
      }
      if (cues.size() + subtitle_packets_.size() > 4096) {
        return Fail(packet->pts, "too many subtitle cues at %.3f s (of %.3f s)",
                    "");
      }
      for (const SubtitleCue& cue : cues) {
        EncodedPacket text;
        text.stream_index = index;
        text.pts = text.dts = cue.start + subtitle_offset;
        text.duration = cue.end - cue.start;
        text.keyframe = true;
        // The shared parser already removed markup. Quote literal tag and
        // entity characters for the WebVTT decoder that follows it.
        std::string payload;
        for (char c : cue.text) {
          if (c == '&') {
            payload += "&amp;";
          } else if (c == '<') {
            payload += "&lt;";
          } else if (c == '>') {
            payload += "&gt;";
          } else {
            payload += c;
          }
        }
        text.buffer = PacketBuffer::Copy(
            reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        auto at = std::upper_bound(
            subtitle_packets_.begin(), subtitle_packets_.end(), text.dts,
            [](int64_t time, const EncodedPacket& p) { return time < p.dts; });
        subtitle_packets_.insert(at, std::move(text));
      }
      continue;
    }
    return DemuxerReadResult::kOk;
  }
}

bool Mp4Demuxer::Seek(int64_t time) {
  subtitle_packets_.clear();
  interrupted_ = false;
  if (!aborted_) {
    source_->ClearInterrupt();
  }
  if (fragmented_) {
    return SeekFragments(time);
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
