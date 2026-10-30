#define LOG_TAG "matroska"

#include "backends/matroska/matroska_demuxer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <numeric>

#include "base/log.h"

namespace lychee {

namespace {

// Element IDs (RFC 9559), with their marker bits.
constexpr uint32_t kEbml = 0x1A45DFA3;
constexpr uint32_t kDocType = 0x4282;
constexpr uint32_t kSegment = 0x18538067;
constexpr uint32_t kSeekHead = 0x114D9B74;
constexpr uint32_t kSeek = 0x4DBB;
constexpr uint32_t kSeekId = 0x53AB;
constexpr uint32_t kSeekPosition = 0x53AC;
constexpr uint32_t kInfo = 0x1549A966;
constexpr uint32_t kTimestampScale = 0x2AD7B1;
constexpr uint32_t kDuration = 0x4489;
constexpr uint32_t kTitle = 0x7BA9;
constexpr uint32_t kTracks = 0x1654AE6B;
constexpr uint32_t kTrackEntry = 0xAE;
constexpr uint32_t kTrackNumber = 0xD7;
constexpr uint32_t kTrackType = 0x83;
constexpr uint32_t kFlagDefault = 0x88;
constexpr uint32_t kFlagForced = 0x55AA;
constexpr uint32_t kDefaultDuration = 0x23E383;
constexpr uint32_t kName = 0x536E;
constexpr uint32_t kLanguage = 0x22B59C;
constexpr uint32_t kLanguageBcp47 = 0x22B59D;
constexpr uint32_t kCodecId = 0x86;
constexpr uint32_t kCodecPrivate = 0x63A2;
constexpr uint32_t kCodecDelay = 0x56AA;
constexpr uint32_t kVideo = 0xE0;
constexpr uint32_t kPixelWidth = 0xB0;
constexpr uint32_t kPixelHeight = 0xBA;
constexpr uint32_t kDisplayWidth = 0x54B0;
constexpr uint32_t kDisplayHeight = 0x54BA;
constexpr uint32_t kDisplayUnit = 0x54B2;
constexpr uint32_t kColour = 0x55B0;
constexpr uint32_t kMatrixCoefficients = 0x55B1;
constexpr uint32_t kRange = 0x55B9;
constexpr uint32_t kTransferCharacteristics = 0x55BA;
constexpr uint32_t kPrimaries = 0x55BB;
constexpr uint32_t kProjection = 0x7670;
constexpr uint32_t kProjectionType = 0x7671;
constexpr uint32_t kProjectionPoseRoll = 0x7675;
constexpr uint32_t kAudio = 0xE1;
constexpr uint32_t kSamplingFrequency = 0xB5;
constexpr uint32_t kOutputSamplingFrequency = 0x78B5;
constexpr uint32_t kChannels = 0x9F;
constexpr uint32_t kContentEncodings = 0x6D80;
constexpr uint32_t kContentEncoding = 0x6240;
constexpr uint32_t kContentEncodingType = 0x5033;
constexpr uint32_t kContentCompression = 0x5034;
constexpr uint32_t kContentCompAlgo = 0x4254;
constexpr uint32_t kContentCompSettings = 0x4255;
constexpr uint32_t kCluster = 0x1F43B675;
constexpr uint32_t kTimestamp = 0xE7;
constexpr uint32_t kSimpleBlock = 0xA3;
constexpr uint32_t kBlockGroup = 0xA0;
constexpr uint32_t kBlock = 0xA1;
constexpr uint32_t kBlockDuration = 0x9B;
constexpr uint32_t kDiscardPadding = 0x75A2;
constexpr uint32_t kReferenceBlock = 0xFB;
constexpr uint32_t kCues = 0x1C53BB6B;
constexpr uint32_t kCuePoint = 0xBB;
constexpr uint32_t kCueTime = 0xB3;
constexpr uint32_t kCueTrackPositions = 0xB7;
constexpr uint32_t kCueTrack = 0xF7;
constexpr uint32_t kCueClusterPosition = 0xF1;
constexpr uint32_t kTags = 0x1254C367;
constexpr uint32_t kTag = 0x7373;
constexpr uint32_t kTargets = 0x63C0;
constexpr uint32_t kTagTrackUid = 0x63C5;
constexpr uint32_t kTagEditionUid = 0x63C9;
constexpr uint32_t kTagChapterUid = 0x63C4;
constexpr uint32_t kTagAttachmentUid = 0x63C6;
constexpr uint32_t kSimpleTag = 0x67C8;
constexpr uint32_t kTagName = 0x45A3;
constexpr uint32_t kTagString = 0x4487;
constexpr uint32_t kAttachments = 0x1941A469;
constexpr uint32_t kAttachedFile = 0x61A7;
constexpr uint32_t kFileName = 0x466E;
constexpr uint32_t kFileMediaType = 0x4660;
constexpr uint32_t kFileData = 0x465C;
constexpr uint32_t kChapters = 0x1043A770;

// Elements read whole into memory: a header element (Info, Tracks, Tags,
// SeekHead) of a real file is a few KB.
constexpr size_t kMaxHeaderSize = 16 << 20;
// Cues of a file with hours of video are a few MB; fonts in Attachments
// tens of MB at most.
constexpr size_t kMaxCuesSize = 64 << 20;
constexpr size_t kMaxAttachmentsSize = 64 << 20;
constexpr size_t kMaxCuePoints = 1 << 22;
constexpr size_t kMaxClusters = 1 << 22;
constexpr size_t kMaxStreams = 256;
// One block: an 8K intra frame is a few MB. A larger size is a corrupt
// file, and allocating it could take the process down.
constexpr int64_t kMaxBlockSize = 64 << 20;
// How far past a damaged element a cluster is looked for.
constexpr int64_t kMaxResync = 16 << 20;
// Packets of a reordering video track (H.264, HEVC) held to find their
// dts: the most frames a decoded picture buffer holds.
constexpr size_t kDtsWindow = 16;
// Packets queued behind one whose dts is not known yet, at most.
constexpr size_t kMaxPending = 1024;
constexpr size_t kCacheSize = 64 << 10;

// An element inside a body already in memory.
struct Child {
  uint32_t id = 0;
  const uint8_t* data = nullptr;
  size_t size = 0;
};

// An EBML variable-length integer at |data[*pos]|. IDs keep their marker
// bits and are at most four bytes. Sizes with every value bit set are
// unknown (|*unknown|).
bool ReadVint(const uint8_t* data,
              size_t size,
              size_t* pos,
              bool id,
              uint64_t* value,
              bool* unknown = nullptr,
              int* length = nullptr) {
  if (*pos >= size || data[*pos] == 0) {
    return false;
  }
  const uint8_t first = data[*pos];
  int bytes = 1;
  while (!(first & (0x80 >> (bytes - 1)))) {
    ++bytes;
  }
  if ((id && bytes > 4) || size - *pos < static_cast<size_t>(bytes)) {
    return false;
  }
  uint64_t v = id ? first : first & (0xff >> bytes);
  bool all_ones = v == (0xffu >> bytes);
  for (int i = 1; i < bytes; ++i) {
    const uint8_t byte = data[*pos + i];
    v = (v << 8) | byte;
    all_ones = all_ones && byte == 0xff;
  }
  *pos += bytes;
  *value = v;
  if (unknown) {
    *unknown = !id && all_ones;
  }
  if (length) {
    *length = bytes;
  }
  return true;
}

// The children of a master element's body. Stops at the first malformed
// one; an unknown size runs to the end of the body.
std::vector<Child> Children(const uint8_t* data, size_t size) {
  std::vector<Child> children;
  size_t pos = 0;
  while (pos < size) {
    uint64_t id = 0;
    uint64_t length = 0;
    bool unknown = false;
    if (!ReadVint(data, size, &pos, true, &id) ||
        !ReadVint(data, size, &pos, false, &length, &unknown)) {
      break;
    }
    if (unknown) {
      length = size - pos;
    }
    if (length > size - pos) {
      break;
    }
    children.push_back(
        {static_cast<uint32_t>(id), data + pos, static_cast<size_t>(length)});
    pos += static_cast<size_t>(length);
  }
  return children;
}

uint64_t UInt(const Child& child) {
  uint64_t value = 0;
  for (size_t i = 0; i < child.size && i < 8; ++i) {
    value = (value << 8) | child.data[i];
  }
  return value;
}

// A big-endian two's complement integer of 1 to 8 bytes.
int64_t SignedInt(const Child& child) {
  if (child.size == 0 || child.size > 8) {
    return 0;
  }
  uint64_t value = UInt(child);
  const unsigned bits = static_cast<unsigned>(child.size) * 8;
  if (bits < 64 && (value >> (bits - 1)) & 1) {
    value |= ~uint64_t{0} << bits;
  }
  return static_cast<int64_t>(value);
}

double Float(const Child& child) {
  if (child.size == 4) {
    uint32_t bits = static_cast<uint32_t>(UInt(child));
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
  }
  if (child.size == 8) {
    uint64_t bits = UInt(child);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
  }
  return 0;
}

std::string String(const Child& child) {
  const auto* text = reinterpret_cast<const char*>(child.data);
  return std::string(text, strnlen(text, child.size));
}

std::vector<uint8_t> Binary(const Child& child) {
  return std::vector<uint8_t>(child.data, child.data + child.size);
}

bool IsTopLevel(uint32_t id) {
  switch (id) {
    case kCluster:
    case kCues:
    case kTags:
    case kAttachments:
    case kChapters:
    case kSeekHead:
    case kInfo:
    case kTracks:
    case kSegment:
    case kEbml:
      return true;
    default:
      return false;
  }
}

// A two-byte AudioSpecificConfig for the old A_AAC/MPEG2/... and
// A_AAC/MPEG4/... codec IDs, which carry no CodecPrivate.
std::vector<uint8_t> AacConfig(const std::string& codec_id,
                               int sample_rate,
                               int channels) {
  int object_type = 2;  // LC
  if (codec_id.find("/MAIN") != std::string::npos) {
    object_type = 1;
  } else if (codec_id.find("/SSR") != std::string::npos) {
    object_type = 3;
  } else if (codec_id.find("/LTP") != std::string::npos) {
    object_type = 4;
  }
  static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                               22050, 16000, 12000, 11025, 8000,  7350};
  int index = 4;
  for (int i = 0; i < 13; ++i) {
    if (kRates[i] == sample_rate) {
      index = i;
    }
  }
  const int config =
      (object_type << 11) | (index << 7) | ((channels & 0xf) << 3);
  return {static_cast<uint8_t>(config >> 8), static_cast<uint8_t>(config)};
}

void MapVideoCodec(const std::string& id, VideoConfig* config) {
  static const struct {
    const char* id;
    VideoCodec codec;
    const char* name;
  } kCodecs[] = {
      {"V_MPEG4/ISO/AVC", VideoCodec::kH264, "h264"},
      {"V_MPEGH/ISO/HEVC", VideoCodec::kHEVC, "hevc"},
      {"V_VP8", VideoCodec::kVP8, "vp8"},
      {"V_VP9", VideoCodec::kVP9, "vp9"},
      {"V_AV1", VideoCodec::kAV1, "av1"},
      {"V_MPEG2", VideoCodec::kMPEG2, "mpeg2video"},
      {"V_MPEG4/ISO/ASP", VideoCodec::kMPEG4, "mpeg4"},
      {"V_MPEG4/ISO/SP", VideoCodec::kMPEG4, "mpeg4"},
      {"V_MPEG4/ISO/AP", VideoCodec::kMPEG4, "mpeg4"},
  };
  for (const auto& codec : kCodecs) {
    if (id == codec.id) {
      config->codec = codec.codec;
      config->codec_name = codec.name;
      return;
    }
  }
  config->codec = VideoCodec::kOther;
  config->codec_name = id;
}

void MapAudioCodec(const std::string& id, AudioConfig* config) {
  static const struct {
    const char* id;
    AudioCodec codec;
    const char* name;
  } kCodecs[] = {
      {"A_OPUS", AudioCodec::kOpus, "opus"},
      {"A_FLAC", AudioCodec::kFLAC, "flac"},
      {"A_MPEG/L3", AudioCodec::kMP3, "mp3"},
      {"A_VORBIS", AudioCodec::kVorbis, "vorbis"},
      {"A_AC3", AudioCodec::kAC3, "ac3"},
      {"A_EAC3", AudioCodec::kEAC3, "eac3"},
      {"A_ALAC", AudioCodec::kALAC, "alac"},
      {"A_PCM/INT/LIT", AudioCodec::kPCM, "pcm"},
  };
  if (id.rfind("A_AAC", 0) == 0) {
    config->codec = AudioCodec::kAAC;
    config->codec_name = "aac";
    return;
  }
  for (const auto& codec : kCodecs) {
    if (id == codec.id) {
      config->codec = codec.codec;
      config->codec_name = codec.name;
      return;
    }
  }
  config->codec = AudioCodec::kOther;
  config->codec_name = id;
}

void MapSubtitleCodec(const std::string& id, SubtitleConfig* config) {
  static const struct {
    const char* id;
    const char* name;
    bool text;
  } kCodecs[] = {
      {"S_TEXT/UTF8", "subrip", true},
      {"S_TEXT/ASCII", "subrip", true},
      {"S_TEXT/ASS", "ass", true},
      {"S_TEXT/SSA", "ass", true},
      {"S_ASS", "ass", true},
      {"S_SSA", "ass", true},
      {"S_TEXT/WEBVTT", "webvtt", true},
      {"D_WEBVTT/SUBTITLES", "webvtt", true},
      {"D_WEBVTT/CAPTIONS", "webvtt", true},
      {"S_HDMV/PGS", "hdmv_pgs_subtitle", false},
      {"S_DVBSUB", "dvb_subtitle", false},
      {"S_VOBSUB", "dvd_subtitle", false},
  };
  for (const auto& codec : kCodecs) {
    if (id == codec.id) {
      config->codec_name = codec.name;
      config->text = codec.text;
      return;
    }
  }
  config->codec_name = id;
}

// The bytes of a SimpleBlock's single frame, read in place: the body is
// followed by kPacketPadding zero bytes.
class BlockBuffer : public PacketBuffer {
 public:
  BlockBuffer(std::shared_ptr<std::vector<uint8_t>> bytes,
              size_t offset,
              size_t size)
      : bytes_(std::move(bytes)) {
    data_ = bytes_->data() + offset;
    size_ = size;
  }

 private:
  std::shared_ptr<std::vector<uint8_t>> bytes_;
};

}  // namespace

struct MatroskaDemuxer::Element {
  uint32_t id = 0;
  int64_t offset = 0;
  int64_t body = 0;
  int64_t size = -1;  // -1: unknown.

  int64_t end() const { return size < 0 ? -1 : body + size; }
};

MatroskaDemuxer::MatroskaDemuxer() = default;
MatroskaDemuxer::~MatroskaDemuxer() = default;

bool MatroskaDemuxer::Open(const std::string& url, std::string* error) {
  std::unique_ptr<DataSource> source = OpenDataSource(url, error);
  if (!source) {
    return false;
  }
  return Open(std::move(source), error);
}

bool MatroskaDemuxer::Open(std::unique_ptr<DataSource> source,
                           std::string* error) {
  {
    std::lock_guard<std::mutex> lock(source_mutex_);
    source_ = std::move(source);
    if (aborted_) {
      source_->Interrupt();
    }
  }
  try {
    return ParseHeaders(error);
  } catch (const std::bad_alloc&) {
    *error = "out of memory for the Matroska headers";
    return false;
  }
}

int64_t MatroskaDemuxer::ReadAt(int64_t offset, uint8_t* data, size_t size) {
  size_t done = 0;
  while (done < size) {
    const int64_t at = offset + static_cast<int64_t>(done);
    if (at >= cache_offset_ &&
        at < cache_offset_ + static_cast<int64_t>(cache_.size())) {
      size_t n = std::min(
          size - done,
          static_cast<size_t>(cache_offset_ +
                              static_cast<int64_t>(cache_.size()) - at));
      memcpy(data + done, cache_.data() + (at - cache_offset_), n);
      done += n;
      continue;
    }
    int64_t n;
    if (size - done >= kCacheSize) {
      n = source_->Read(at, data + done, size - done);
      if (n > 0) {
        done += static_cast<size_t>(n);
      }
    } else {
      cache_.resize(kCacheSize);
      n = source_->Read(at, cache_.data(), cache_.size());
      cache_.resize(n > 0 ? static_cast<size_t>(n) : 0);
      cache_offset_ = at;
    }
    if (n < 0) {
      return -1;
    }
    if (n == 0) {
      break;
    }
  }
  return static_cast<int64_t>(done);
}

MatroskaDemuxer::Status MatroskaDemuxer::ReadElementHeader(int64_t offset,
                                                           Element* element) {
  uint8_t header[12];
  int64_t got = ReadAt(offset, header, sizeof(header));
  if (got < 0) {
    return Status::kError;
  }
  size_t pos = 0;
  uint64_t id = 0;
  uint64_t size = 0;
  bool unknown = false;
  if (!ReadVint(header, static_cast<size_t>(got), &pos, true, &id) ||
      !ReadVint(header, static_cast<size_t>(got), &pos, false, &size,
                &unknown)) {
    // Too few bytes for a header: the data ended (in the middle of one,
    // if any were there).
    bool short_read = got < static_cast<int64_t>(sizeof(header));
    return short_read && (got == 0 || header[0] != 0) ? Status::kEnd
                                                      : Status::kInvalid;
  }
  element->id = static_cast<uint32_t>(id);
  element->offset = offset;
  element->body = offset + static_cast<int64_t>(pos);
  // Sizes past what any file holds are as good as unknown, and are not.
  if (!unknown && size > (uint64_t{1} << 56)) {
    return Status::kInvalid;
  }
  element->size = unknown ? -1 : static_cast<int64_t>(size);
  return Status::kOk;
}

MatroskaDemuxer::Status MatroskaDemuxer::ReadBody(const Element& element,
                                                  size_t limit,
                                                  std::vector<uint8_t>* body,
                                                  std::string* error) {
  if (element.size < 0 || static_cast<uint64_t>(element.size) > limit) {
    if (error) {
      char message[96];
      snprintf(message, sizeof(message), "element %X too large (%lld bytes)",
               element.id, static_cast<long long>(element.size));
      *error = message;
    }
    return Status::kInvalid;
  }
  // Past the end of the file: as truncated as a read that ends early,
  // without allocating for it.
  if (file_size_ > 0 && element.end() > file_size_) {
    return Status::kEnd;
  }
  body->resize(static_cast<size_t>(element.size));
  int64_t got = ReadAt(element.body, body->data(), body->size());
  if (got < 0) {
    return Status::kError;
  }
  return got < element.size ? Status::kEnd : Status::kOk;
}

bool MatroskaDemuxer::ParseHeaders(std::string* error) {
  file_size_ = -1;
  Element element;
  Status status = ReadElementHeader(0, &element);
  // Known once the first bytes arrived (HTTP).
  file_size_ = source_->size();
  if (status == Status::kError) {
    *error = source_->error();
    return false;
  }
  if (status != Status::kOk || element.id != kEbml) {
    *error = kNotMatroska;
    return false;
  }
  std::vector<uint8_t> body;
  if (ReadBody(element, 4096, &body, nullptr) != Status::kOk) {
    *error = "malformed EBML header";
    return false;
  }
  std::string doc_type = "matroska";
  for (const Child& child : Children(body.data(), body.size())) {
    if (child.id == kDocType) {
      doc_type = String(child);
    }
  }
  if (doc_type != "matroska" && doc_type != "webm") {
    *error = std::string(kNotMatroska) + ": " + doc_type;
    return false;
  }
  media_info_ = MediaInfo();
  media_info_.format_name = "matroska,webm";

  // The Segment, after anything (Void) in between.
  int64_t position = element.end();
  for (int i = 0;; ++i) {
    status = ReadElementHeader(position, &element);
    if (status != Status::kOk || i > 16 ||
        (element.id != kSegment && element.size < 0)) {
      *error =
          status == Status::kError ? source_->error() : "no Matroska segment";
      return false;
    }
    if (element.id == kSegment) {
      break;
    }
    position = element.end();
  }
  segment_start_ = element.body;
  segment_end_ = element.end();

  // Header elements up to the first cluster. Those after the clusters
  // (Cues, often Tags) are found through the SeekHead.
  bool have_info = false;
  for (position = segment_start_;;) {
    if (aborted_) {
      *error = "aborted";
      return false;
    }
    if (segment_end_ >= 0 && position >= segment_end_) {
      break;
    }
    status = ReadElementHeader(position, &element);
    if (status == Status::kEnd) {
      break;
    }
    if (status == Status::kError) {
      *error = source_->error();
      return false;
    }
    if (status == Status::kInvalid || element.size < 0) {
      if (status == Status::kOk && element.id == kCluster) {
        first_cluster_ = position;
        break;
      }
      *error = "malformed Matroska header";
      return false;
    }
    if (element.id == kCluster) {
      first_cluster_ = position;
      break;
    }
    const uint32_t id = element.id;
    if (id == kSeekHead || id == kInfo || id == kTracks || id == kTags ||
        id == kAttachments || id == kCues) {
      size_t limit = id == kCues          ? kMaxCuesSize
                     : id == kAttachments ? kMaxAttachmentsSize
                                          : kMaxHeaderSize;
      if (static_cast<uint64_t>(element.size) > limit) {
        if (id == kTracks || id == kInfo) {
          ReadBody(element, limit, &body, error);
          return false;
        }
        LOGW("element %X of %lld bytes skipped", id,
             static_cast<long long>(element.size));
      } else {
        status = ReadBody(element, limit, &body, error);
        if (status != Status::kOk) {
          if (status == Status::kError) {
            *error = source_->error();
          } else if (status == Status::kEnd) {
            *error = "truncated Matroska header";
          }
          return false;
        }
        if (id == kSeekHead) {
          ParseSeekHead(body);
        } else if (id == kInfo) {
          ParseInfo(body);
          have_info = true;
        } else if (id == kTracks) {
          if (!ParseTracks(body, error)) {
            return false;
          }
        } else if (id == kTags) {
          ParseTags(body);
        } else if (id == kAttachments) {
          ParseAttachments(body);
        } else {
          ParseCues(body);
          cues_loaded_ = true;
        }
      }
    }
    position = element.end();
  }
  // Header elements behind the clusters, where the file can be read out
  // of order.
  if (file_size_ > 0) {
    // ReadElementAt() may add positions (a second SeekHead).
    for (size_t i = 0; i < seek_positions_.size() && i < 16; ++i) {
      ReadElementAt(seek_positions_[i]);
    }
  }
  if (media_info_.streams.empty()) {
    *error = "no video, audio or subtitle track";
    return false;
  }
  if (!have_info) {
    LOGW("no Info element: timestamps in milliseconds");
  }
  // The first cluster's timestamp is where the presentation starts.
  if (first_cluster_ >= 0 &&
      ReadElementHeader(first_cluster_, &element) == Status::kOk) {
    uint8_t head[32];
    int64_t got = ReadAt(element.body, head, sizeof(head));
    if (got > 0) {
      std::vector<Child> children = Children(head, static_cast<size_t>(got));
      if (!children.empty() && children[0].id == kTimestamp) {
        media_info_.start_time = std::max<int64_t>(
            ToMicros(static_cast<int64_t>(UInt(children[0]))), 0);
      }
    }
  }
  media_info_.seekable = file_size_ > 0 && first_cluster_ >= 0;
  position_ = first_cluster_ >= 0
                  ? first_cluster_
                  : (segment_end_ >= 0 ? segment_end_ : position);
  in_cluster_ = false;
  LOGI("%s: %zu tracks, duration %.3f s, %s", doc_type.c_str(),
       media_info_.streams.size(),
       media_info_.duration == kNoTimestamp ? -1.0 : media_info_.duration / 1e6,
       cues_loaded_ || cues_position_ >= 0 ? "cues" : "no cues");
  return true;
}

void MatroskaDemuxer::ParseSeekHead(const std::vector<uint8_t>& body) {
  for (const Child& seek : Children(body.data(), body.size())) {
    if (seek.id != kSeek) {
      continue;
    }
    uint32_t id = 0;
    int64_t position = -1;
    for (const Child& child : Children(seek.data, seek.size)) {
      if (child.id == kSeekId) {
        id = static_cast<uint32_t>(UInt(child));
      } else if (child.id == kSeekPosition) {
        uint64_t value = UInt(child);
        if (value < (uint64_t{1} << 56)) {
          position = segment_start_ + static_cast<int64_t>(value);
        }
      }
    }
    if (position < 0) {
      continue;
    }
    if (id == kCues) {
      cues_position_ = position;
    } else if ((id == kTags || id == kAttachments || id == kSeekHead ||
                id == kTracks || id == kInfo) &&
               std::find(seek_positions_.begin(), seek_positions_.end(),
                         position) == seek_positions_.end()) {
      seek_positions_.push_back(position);
    }
  }
}

void MatroskaDemuxer::ReadElementAt(int64_t position) {
  Element element;
  if (position < segment_start_ || (file_size_ > 0 && position >= file_size_) ||
      ReadElementHeader(position, &element) != Status::kOk) {
    return;
  }
  const uint32_t id = element.id;
  const bool wanted = (id == kTags && !tags_read_) ||
                      (id == kAttachments && !attachments_read_) ||
                      (id == kSeekHead) ||
                      (id == kTracks && media_info_.streams.empty());
  if (!wanted) {
    return;
  }
  std::vector<uint8_t> body;
  size_t limit = id == kAttachments ? kMaxAttachmentsSize : kMaxHeaderSize;
  if (ReadBody(element, limit, &body, nullptr) != Status::kOk) {
    LOGW("element %X at %lld not read", id, static_cast<long long>(position));
    return;
  }
  if (id == kTags) {
    ParseTags(body);
  } else if (id == kAttachments) {
    ParseAttachments(body);
  } else if (id == kSeekHead) {
    ParseSeekHead(body);
  } else {
    std::string error;
    ParseTracks(body, &error);
  }
}

void MatroskaDemuxer::ParseInfo(const std::vector<uint8_t>& body) {
  double duration = -1;
  for (const Child& child : Children(body.data(), body.size())) {
    if (child.id == kTimestampScale) {
      uint64_t scale = UInt(child);
      // Up to a second per tick; 0 is invalid.
      if (scale > 0 && scale <= 1000000000) {
        timestamp_scale_ = static_cast<int64_t>(scale);
      }
    } else if (child.id == kDuration) {
      duration = Float(child);
    } else if (child.id == kTitle) {
      std::string title = String(child);
      if (!title.empty()) {
        media_info_.metadata.emplace_back("title", std::move(title));
      }
    }
  }
  // Ticks to microseconds; a negative, NaN or absurd value is unknown.
  double micros = duration * static_cast<double>(timestamp_scale_) / 1000.0;
  if (duration > 0 && micros < 1e15) {
    media_info_.duration = static_cast<int64_t>(std::llround(micros));
  }
}

bool MatroskaDemuxer::ParseTracks(const std::vector<uint8_t>& body,
                                  std::string* error) {
  for (const Child& entry : Children(body.data(), body.size())) {
    if (entry.id == kTrackEntry && media_info_.streams.size() < kMaxStreams) {
      ParseTrackEntry(entry.data, entry.size);
    }
  }
  if (media_info_.streams.empty()) {
    *error = "no video, audio or subtitle track";
    return false;
  }
  return true;
}

bool MatroskaDemuxer::ParseTrackEntry(const uint8_t* data, size_t size) {
  uint64_t number = 0;
  uint64_t type = 0;
  bool flag_default = true;
  bool forced = false;
  int64_t default_duration = 0;
  int64_t codec_delay = 0;
  std::string name;
  std::string language = "eng";
  std::string language_bcp47;
  std::string codec_id;
  std::vector<uint8_t> codec_private;
  const Child* video = nullptr;
  const Child* audio = nullptr;
  std::vector<uint8_t> stripped_header;
  std::vector<Child> children = Children(data, size);
  for (const Child& child : children) {
    switch (child.id) {
      case kTrackNumber:
        number = UInt(child);
        break;
      case kTrackType:
        type = UInt(child);
        break;
      case kFlagDefault:
        flag_default = UInt(child) != 0;
        break;
      case kFlagForced:
        forced = UInt(child) != 0;
        break;
      case kDefaultDuration:
        default_duration =
            static_cast<int64_t>(std::min<uint64_t>(UInt(child), 1ull << 40));
        break;
      case kName:
        name = String(child);
        break;
      case kLanguage:
        language = String(child);
        break;
      case kLanguageBcp47:
        language_bcp47 = String(child);
        break;
      case kCodecId:
        codec_id = String(child);
        break;
      case kCodecPrivate:
        codec_private = Binary(child);
        break;
      case kCodecDelay:
        codec_delay =
            static_cast<int64_t>(std::min<uint64_t>(UInt(child), 1ull << 40));
        break;
      case kVideo:
        video = &child;
        break;
      case kAudio:
        audio = &child;
        break;
      case kContentEncodings:
        for (const Child& encoding : Children(child.data, child.size)) {
          if (encoding.id != kContentEncoding) {
            continue;
          }
          uint64_t encoding_type = 0;
          uint64_t algorithm = 0;
          bool compression = false;
          for (const Child& field : Children(encoding.data, encoding.size)) {
            if (field.id == kContentEncodingType) {
              encoding_type = UInt(field);
            } else if (field.id == kContentCompression) {
              compression = true;
              for (const Child& c : Children(field.data, field.size)) {
                if (c.id == kContentCompAlgo) {
                  algorithm = UInt(c);
                } else if (c.id == kContentCompSettings) {
                  stripped_header = Binary(c);
                }
              }
            }
          }
          // Only header stripping: zlib, bzlib and LZO need libraries, and
          // encrypted tracks keys.
          if (encoding_type != 0 || !compression || algorithm != 3) {
            LOGW("track %llu: %s; skipped",
                 static_cast<unsigned long long>(number),
                 encoding_type != 0 ? "encrypted" : "compressed");
            return false;
          }
        }
        break;
      default:
        break;
    }
  }
  if (number == 0 || TrackByNumber(number)) {
    return false;
  }

  StreamInfo info;
  info.index = static_cast<int>(media_info_.streams.size());
  info.title = name;
  std::string lang = language_bcp47.empty() ? language : language_bcp47;
  if (lang != "und") {
    info.language = lang;
  }
  info.default_track = flag_default || forced;
  Track track;
  track.number = number;
  track.stream = info.index;
  track.default_duration = default_duration;
  track.stripped_header = std::move(stripped_header);

  if (type == 1) {
    info.type = MediaType::kVideo;
    VideoConfig& config = info.video;
    MapVideoCodec(codec_id, &config);
    config.extradata = std::move(codec_private);
    int64_t display_width = 0;
    int64_t display_height = 0;
    uint64_t display_unit = 0;
    if (video) {
      for (const Child& child : Children(video->data, video->size)) {
        switch (child.id) {
          case kPixelWidth:
            config.width =
                static_cast<int>(std::min<uint64_t>(UInt(child), 1 << 16));
            break;
          case kPixelHeight:
            config.height =
                static_cast<int>(std::min<uint64_t>(UInt(child), 1 << 16));
            break;
          case kDisplayWidth:
            display_width =
                static_cast<int64_t>(std::min<uint64_t>(UInt(child), 1 << 16));
            break;
          case kDisplayHeight:
            display_height =
                static_cast<int64_t>(std::min<uint64_t>(UInt(child), 1 << 16));
            break;
          case kDisplayUnit:
            display_unit = UInt(child);
            break;
          case kColour:
            for (const Child& c : Children(child.data, child.size)) {
              uint64_t value = UInt(c);
              if (c.id == kMatrixCoefficients && value < 256) {
                config.color.matrix = static_cast<uint8_t>(value);
              } else if (c.id == kTransferCharacteristics && value < 256) {
                config.color.transfer = static_cast<uint8_t>(value);
              } else if (c.id == kPrimaries && value < 256) {
                config.color.primaries = static_cast<uint8_t>(value);
              } else if (c.id == kRange) {
                config.color.full_range = value == 2;
              }
            }
            break;
          case kProjection: {
            uint64_t projection = 0;
            double roll = 0;
            for (const Child& c : Children(child.data, child.size)) {
              if (c.id == kProjectionType) {
                projection = UInt(c);
              } else if (c.id == kProjectionPoseRoll) {
                roll = Float(c);
              }
            }
            // Rectangular video turned by the roll, counter-clockwise.
            if (projection == 0 && std::isfinite(roll)) {
              int degrees = static_cast<int>(std::lround(-roll / 90.0)) * 90;
              config.rotation = ((degrees % 360) + 360) % 360;
            }
            break;
          }
          default:
            break;
        }
      }
    }
    if (display_unit == 0 && display_width > 0 && display_height > 0 &&
        config.width > 0 && config.height > 0) {
      int64_t num = display_width * config.height;
      int64_t den = display_height * config.width;
      int64_t gcd = std::gcd(num, den);
      if (gcd > 0 && num != den) {
        config.sar_num = static_cast<int>(num / gcd);
        config.sar_den = static_cast<int>(den / gcd);
      }
    }
    track.needs_dts =
        config.codec == VideoCodec::kH264 || config.codec == VideoCodec::kHEVC;
  } else if (type == 2) {
    info.type = MediaType::kAudio;
    AudioConfig& config = info.audio;
    MapAudioCodec(codec_id, &config);
    double rate = 8000;
    double output_rate = 0;
    uint64_t channels = 1;
    if (audio) {
      for (const Child& child : Children(audio->data, audio->size)) {
        if (child.id == kSamplingFrequency) {
          rate = Float(child);
        } else if (child.id == kOutputSamplingFrequency) {
          output_rate = Float(child);
        } else if (child.id == kChannels) {
          channels = UInt(child);
        }
      }
    }
    if (!(rate > 0 && rate < 1e7)) {
      rate = 0;
    }
    config.sample_rate = static_cast<int>(rate);
    config.channels = static_cast<int>(std::min<uint64_t>(channels, 64));
    if (config.codec == AudioCodec::kOpus) {
      // Opus always decodes at 48 kHz.
      config.sample_rate = 48000;
    }
    if (config.codec == AudioCodec::kAAC && codec_private.empty()) {
      codec_private = AacConfig(codec_id, config.sample_rate, config.channels);
      if (codec_id.find("SBR") != std::string::npos && output_rate > 0) {
        config.sample_rate = static_cast<int>(output_rate);
      }
    }
    config.extradata = std::move(codec_private);
    track.codec_delay = codec_delay / 1000;
    track.audio = true;
  } else if (type == 0x11) {
    info.type = MediaType::kSubtitle;
    MapSubtitleCodec(codec_id, &info.subtitle);
    info.subtitle.extradata = std::move(codec_private);
    track.webvtt_lines = codec_id.rfind("D_WEBVTT/", 0) == 0;
  } else {
    return false;
  }

  // The first track of each type the file marks default, else the first.
  auto pick = [&](int* chosen) {
    if (*chosen < 0 ||
        (info.default_track && !media_info_.streams[*chosen].default_track)) {
      *chosen = info.index;
    }
  };
  if (info.type == MediaType::kVideo) {
    pick(&media_info_.default_video_stream);
  } else if (info.type == MediaType::kAudio) {
    pick(&media_info_.default_audio_stream);
  } else if (info.default_track && media_info_.default_subtitle_stream < 0) {
    media_info_.default_subtitle_stream = info.index;
  }
  media_info_.streams.push_back(std::move(info));
  tracks_.push_back(std::move(track));
  return true;
}

void MatroskaDemuxer::ParseTags(const std::vector<uint8_t>& body) {
  tags_read_ = true;
  for (const Child& tag : Children(body.data(), body.size())) {
    if (tag.id != kTag) {
      continue;
    }
    // Only tags about the whole file: no track, edition, chapter or
    // attachment targets.
    bool global = true;
    std::vector<Child> fields = Children(tag.data, tag.size);
    for (const Child& field : fields) {
      if (field.id != kTargets) {
        continue;
      }
      for (const Child& target : Children(field.data, field.size)) {
        if (target.id == kTagTrackUid || target.id == kTagEditionUid ||
            target.id == kTagChapterUid || target.id == kTagAttachmentUid) {
          global = false;
        }
      }
    }
    if (!global) {
      continue;
    }
    for (const Child& field : fields) {
      if (field.id != kSimpleTag) {
        continue;
      }
      std::string key;
      std::string value;
      for (const Child& c : Children(field.data, field.size)) {
        if (c.id == kTagName) {
          key = String(c);
        } else if (c.id == kTagString) {
          value = String(c);
        }
      }
      if (key.empty() || value.empty()) {
        continue;
      }
      std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      auto same =
          std::find_if(media_info_.metadata.begin(), media_info_.metadata.end(),
                       [&](const auto& entry) { return entry.first == key; });
      if (same == media_info_.metadata.end()) {
        media_info_.metadata.emplace_back(std::move(key), std::move(value));
      }
    }
  }
}

void MatroskaDemuxer::ParseAttachments(const std::vector<uint8_t>& body) {
  attachments_read_ = true;
  for (const Child& file : Children(body.data(), body.size())) {
    if (file.id != kAttachedFile) {
      continue;
    }
    std::string name;
    std::string media_type;
    const Child* data = nullptr;
    std::vector<Child> fields = Children(file.data, file.size);
    for (const Child& field : fields) {
      if (field.id == kFileName) {
        name = String(field);
      } else if (field.id == kFileMediaType) {
        media_type = String(field);
      } else if (field.id == kFileData) {
        data = &field;
      }
    }
    if (!data || data->size == 0) {
      continue;
    }
    auto bytes = std::make_shared<const std::vector<uint8_t>>(
        data->data, data->data + data->size);
    if ((media_type == "image/jpeg" || media_type == "image/png") &&
        name.rfind("cover", 0) == 0 && !media_info_.artwork) {
      // Matroska's convention for cover art.
      media_info_.artwork = std::move(bytes);
    }
  }
}

void MatroskaDemuxer::ParseCues(const std::vector<uint8_t>& body) {
  cues_.clear();
  for (const Child& point : Children(body.data(), body.size())) {
    if (point.id != kCuePoint) {
      continue;
    }
    int64_t time = -1;
    std::vector<Child> fields = Children(point.data, point.size);
    for (const Child& field : fields) {
      if (field.id == kCueTime) {
        time =
            static_cast<int64_t>(std::min<uint64_t>(UInt(field), 1ull << 52));
      }
    }
    if (time < 0) {
      continue;
    }
    for (const Child& field : fields) {
      if (field.id != kCueTrackPositions || cues_.size() >= kMaxCuePoints) {
        continue;
      }
      uint64_t track = 0;
      uint64_t position = UINT64_MAX;
      for (const Child& c : Children(field.data, field.size)) {
        if (c.id == kCueTrack) {
          track = UInt(c);
        } else if (c.id == kCueClusterPosition) {
          position = UInt(c);
        }
      }
      if (position >= (uint64_t{1} << 56)) {
        continue;
      }
      int64_t absolute = segment_start_ + static_cast<int64_t>(position);
      if (file_size_ > 0 && absolute >= file_size_) {
        continue;
      }
      cues_.push_back({ToMicros(time), track, absolute});
    }
  }
  std::stable_sort(
      cues_.begin(), cues_.end(),
      [](const CuePoint& a, const CuePoint& b) { return a.time < b.time; });
}

void MatroskaDemuxer::LoadCues() {
  if (cues_loaded_ || cues_position_ < 0) {
    return;
  }
  Element element;
  std::vector<uint8_t> body;
  Status status = ReadElementHeader(cues_position_, &element);
  if (status == Status::kOk && element.id == kCues) {
    status = ReadBody(element, kMaxCuesSize, &body, nullptr);
  } else if (status == Status::kOk) {
    status = Status::kInvalid;
  }
  if (status == Status::kError && (aborted_ || interrupted_)) {
    // Tried again at the next seek.
    return;
  }
  cues_loaded_ = true;
  if (status != Status::kOk) {
    LOGW("cues at %lld not read; seeks scan the clusters",
         static_cast<long long>(cues_position_));
    return;
  }
  ParseCues(body);
}

void MatroskaDemuxer::IndexClustersUpTo(int64_t time) {
  if (clusters_complete_ ||
      (!clusters_.empty() && clusters_.back().time > time)) {
    return;
  }
  int64_t position = clusters_.empty() ? first_cluster_ : clusters_.back().next;
  while (position >= 0 && clusters_.size() < kMaxClusters) {
    if (aborted_ || interrupted_) {
      return;
    }
    Element element;
    if ((segment_end_ >= 0 && position >= segment_end_) ||
        ReadElementHeader(position, &element) != Status::kOk) {
      break;
    }
    if (element.id != kCluster) {
      if (element.size < 0) {
        break;
      }
      position = element.end();
      continue;
    }
    uint8_t head[32];
    int64_t got = ReadAt(element.body, head, sizeof(head));
    std::vector<Child> children =
        Children(head, got > 0 ? static_cast<size_t>(got) : 0);
    if (children.empty() || children[0].id != kTimestamp) {
      break;
    }
    ClusterEntry entry = {
        position, ToMicros(static_cast<int64_t>(UInt(children[0]))), -1};
    if (element.size >= 0) {
      entry.next = element.end();
    } else {
      // A cluster of unknown size (live recordings) ends where an element
      // that cannot be in it starts: walk its children.
      Element child;
      int64_t at = element.body;
      Status status;
      while ((status = ReadElementHeader(at, &child)) == Status::kOk &&
             !IsTopLevel(child.id) && child.size >= 0) {
        if (aborted_ || interrupted_) {
          return;
        }
        at = child.end();
      }
      entry.next = status == Status::kOk && IsTopLevel(child.id) ? at : -1;
    }
    clusters_.push_back(entry);
    if (entry.time > time) {
      return;
    }
    position = entry.next;
  }
  clusters_complete_ = true;
}

int64_t MatroskaDemuxer::ToMicros(int64_t ticks) const {
  // Nanoseconds may overflow; split like Mp4Demuxer's Rescale, and clamp
  // to 2^33 s as it does, so that times added up cannot overflow.
  constexpr int64_t kMaxMicros = (int64_t{1} << 33) * kMicrosPerSecond;
  const int64_t limit = kMaxMicros / timestamp_scale_ * 1000;
  ticks = std::clamp(ticks, -limit, limit);
  return ticks / 1000 * timestamp_scale_ +
         ticks % 1000 * timestamp_scale_ / 1000;
}

MatroskaDemuxer::Track* MatroskaDemuxer::TrackByNumber(uint64_t number) {
  for (Track& track : tracks_) {
    if (track.number == number) {
      return &track;
    }
  }
  return nullptr;
}

void MatroskaDemuxer::SetEnabledStreams(
    const std::vector<int>& stream_indices) {
  for (Track& track : tracks_) {
    track.enabled = std::find(stream_indices.begin(), stream_indices.end(),
                              track.stream) != stream_indices.end();
  }
  pending_.erase(
      std::remove_if(pending_.begin(), pending_.end(),
                     [this](const Pending& pending) {
                       return !tracks_[pending.packet.stream_index].enabled;
                     }),
      pending_.end());
}

DemuxerReadResult MatroskaDemuxer::Fail(const char* what,
                                        const char* detail,
                                        int64_t time) {
  char message[200];
  const double at = (time - media_info_.start_time) / 1e6;
  if (media_info_.duration != kNoTimestamp) {
    snprintf(message, sizeof(message), "%s at %.3f s (of %.3f s)%s", what, at,
             media_info_.duration / 1e6, detail);
  } else {
    snprintf(message, sizeof(message), "%s at %.3f s%s", what, at, detail);
  }
  error_ = message;
  LOGE("%s", message);
  return DemuxerReadResult::kError;
}

DemuxerReadResult MatroskaDemuxer::Read(EncodedPacket* packet) {
  for (;;) {
    if (aborted_ || interrupted_) {
      return DemuxerReadResult::kAborted;
    }
    if (!pending_.empty() && !pending_.front().unresolved) {
      *packet = std::move(pending_.front().packet);
      pending_.pop_front();
      return DemuxerReadResult::kOk;
    }
    if (input_done_) {
      if (!pending_.empty()) {
        ResolveDts(true);
        continue;
      }
      return done_result_;
    }
    DemuxerReadResult result;
    try {
      result = ParseNext();
    } catch (const std::bad_alloc&) {
      result = Fail("out of memory for the block", "", last_time_);
    }
    if (result == DemuxerReadResult::kAborted) {
      return result;
    }
    if (result == DemuxerReadResult::kOk) {
      // A video track that ends long before the audio must not hold the
      // rest of the file back.
      ResolveDts(pending_.size() > kMaxPending);
      continue;
    }
    input_done_ = true;
    done_result_ = result;
  }
}

DemuxerReadResult MatroskaDemuxer::ParseNext() {
  auto aborted = [this] {
    return aborted_ || interrupted_ || source_->interrupted();
  };
  auto ended = [&](int64_t time) {
    if (!source_->error().empty()) {
      LOGW("read failed: %s", source_->error().c_str());
    }
    return Fail("input ended", " (truncated file or lost connection)", time);
  };
  for (;;) {
    if (aborted_ || interrupted_) {
      return DemuxerReadResult::kAborted;
    }
    if (in_cluster_ && cluster_end_ >= 0 && position_ >= cluster_end_) {
      in_cluster_ = false;
    }
    if (segment_end_ >= 0 && position_ >= segment_end_) {
      return DemuxerReadResult::kEndOfStream;
    }
    Element element;
    Status status = ReadElementHeader(position_, &element);
    if (status == Status::kError) {
      return aborted() ? DemuxerReadResult::kAborted : ended(last_time_);
    }
    if (status == Status::kEnd) {
      // The data ends where the segment or cluster says there is more.
      if ((segment_end_ >= 0 && position_ < segment_end_) ||
          (in_cluster_ && cluster_end_ >= 0 && position_ < cluster_end_)) {
        return ended(last_time_);
      }
      return DemuxerReadResult::kEndOfStream;
    }
    bool valid = status == Status::kOk;
    if (valid && element.id == kCluster) {
      if (in_cluster_ && cluster_end_ >= 0) {
        valid = false;  // A cluster inside a cluster.
      } else {
        in_cluster_ = true;
        cluster_end_ = element.end();
        cluster_time_ = 0;
        position_ = element.body;
        continue;
      }
    }
    if (valid && in_cluster_ && IsTopLevel(element.id)) {
      if (cluster_end_ < 0) {
        // The end of a cluster of unknown size.
        in_cluster_ = false;
        continue;
      }
      valid = false;
    }
    if (valid && element.size < 0) {
      valid = false;
    }
    if (valid && in_cluster_ && cluster_end_ >= 0 &&
        element.end() > cluster_end_) {
      valid = false;
    }
    if (!valid) {
      // Damaged data: go on at the next cluster.
      uint8_t window[4096];
      int64_t from = position_ + 1;
      int64_t found = -1;
      while (found < 0 && from - position_ < kMaxResync) {
        int64_t got = ReadAt(from, window, sizeof(window));
        if (got < 0) {
          return aborted() ? DemuxerReadResult::kAborted : ended(last_time_);
        }
        if (got < 4) {
          break;
        }
        for (int64_t i = 0; i + 4 <= got; ++i) {
          if (window[i] == 0x1F && window[i + 1] == 0x43 &&
              window[i + 2] == 0xB6 && window[i + 3] == 0x75) {
            found = from + i;
            break;
          }
        }
        from += got - 3;
      }
      if (found < 0) {
        return Fail("corrupt data", "", last_time_);
      }
      LOGW("damaged data at %lld; going on at the cluster at %lld",
           static_cast<long long>(position_), static_cast<long long>(found));
      position_ = found;
      in_cluster_ = false;
      continue;
    }
    if (!in_cluster_) {
      // Cues, Tags, Void and the like between clusters.
      position_ = element.end();
      continue;
    }

    if (element.id == kTimestamp) {
      uint8_t bytes[8];
      size_t n = static_cast<size_t>(std::min<int64_t>(element.size, 8));
      int64_t got = ReadAt(element.body, bytes, n);
      if (got < static_cast<int64_t>(n)) {
        return got < 0 && aborted() ? DemuxerReadResult::kAborted
                                    : ended(last_time_);
      }
      cluster_time_ = static_cast<int64_t>(
          std::min<uint64_t>(UInt({kTimestamp, bytes, n}), 1ull << 52));
    } else if (element.id == kSimpleBlock || element.id == kBlockGroup) {
      if (element.size > kMaxBlockSize) {
        return Fail("block", " is too large: corrupt file", last_time_);
      }
      // Blocks of tracks not read are skipped unread.
      if (element.id == kSimpleBlock) {
        uint8_t head[8];
        int64_t got =
            ReadAt(element.body, head,
                   static_cast<size_t>(std::min<int64_t>(element.size, 8)));
        size_t pos = 0;
        uint64_t number = 0;
        if (got > 0 &&
            ReadVint(head, static_cast<size_t>(got), &pos, false, &number)) {
          Track* track = TrackByNumber(number);
          if (!track || !track->enabled) {
            position_ = element.end();
            continue;
          }
        }
      }
      auto bytes = std::make_shared<std::vector<uint8_t>>();
      bytes->reserve(static_cast<size_t>(element.size) + kPacketPadding);
      Status read = ReadBody(element, static_cast<size_t>(kMaxBlockSize),
                             bytes.get(), nullptr);
      if (read == Status::kError) {
        return aborted() ? DemuxerReadResult::kAborted : ended(last_time_);
      }
      if (read != Status::kOk) {
        return ended(last_time_);
      }
      if (element.id == kSimpleBlock) {
        bytes->resize(bytes->size() + kPacketPadding, 0);
        ParseBlock(bytes, static_cast<size_t>(element.size), true, false, -1);
      } else {
        const Child* block = nullptr;
        bool reference = false;
        int64_t duration = -1;
        int64_t discard_padding = 0;
        std::vector<Child> children = Children(bytes->data(), bytes->size());
        for (const Child& child : children) {
          if (child.id == kBlock) {
            block = &child;
          } else if (child.id == kReferenceBlock) {
            reference = true;
          } else if (child.id == kBlockDuration) {
            duration = static_cast<int64_t>(
                std::min<uint64_t>(UInt(child), 1ull << 52));
          } else if (child.id == kDiscardPadding) {
            discard_padding = SignedInt(child);
          }
        }
        if (block) {
          auto copy = std::make_shared<std::vector<uint8_t>>(
              block->data, block->data + block->size);
          copy->resize(block->size + kPacketPadding, 0);
          ParseBlock(copy, block->size, false, reference, duration,
                     discard_padding);
        }
      }
    }
    position_ = element.end();
    if (!pending_.empty()) {
      return DemuxerReadResult::kOk;
    }
  }
}

void MatroskaDemuxer::ParseBlock(
    const std::shared_ptr<std::vector<uint8_t>>& bytes,
    size_t size,
    bool simple,
    bool reference,
    int64_t duration_ticks,
    int64_t discard_padding) {
  const uint8_t* data = bytes->data();
  size_t pos = 0;
  uint64_t number = 0;
  if (!ReadVint(data, size, &pos, false, &number) || size - pos < 3) {
    LOGW("malformed block skipped");
    return;
  }
  Track* track = TrackByNumber(number);
  if (!track || !track->enabled) {
    return;
  }
  const auto relative = static_cast<int16_t>((data[pos] << 8) | data[pos + 1]);
  const uint8_t flags = data[pos + 2];
  pos += 3;
  const bool keyframe = simple ? (flags & 0x80) != 0 : !reference;
  const StreamInfo& info = media_info_.streams[track->stream];
  if (track->skip_to_keyframe && info.type == MediaType::kVideo) {
    if (!keyframe) {
      return;
    }
    track->skip_to_keyframe = false;
  }

  // Frame sizes, by lacing: none, Xiph, fixed or EBML.
  std::vector<size_t> sizes;
  const int lacing = (flags >> 1) & 3;
  if (lacing == 0) {
    sizes.push_back(size - pos);
  } else {
    if (pos >= size) {
      LOGW("malformed lacing skipped");
      return;
    }
    const size_t count = data[pos++] + 1u;
    size_t total = 0;
    bool ok = true;
    if (lacing == 1) {
      for (size_t i = 0; i + 1 < count && ok; ++i) {
        size_t frame = 0;
        uint8_t byte;
        do {
          ok = pos < size;
          byte = ok ? data[pos++] : 0;
          frame += byte;
        } while (ok && byte == 255);
        sizes.push_back(frame);
        total += frame;
      }
    } else if (lacing == 3 && count > 1) {
      uint64_t first = 0;
      ok = ReadVint(data, size, &pos, false, &first);
      int64_t frame = static_cast<int64_t>(std::min<uint64_t>(first, size));
      if (ok) {
        sizes.push_back(static_cast<size_t>(frame));
        total += static_cast<size_t>(frame);
      }
      for (size_t i = 1; i + 1 < count && ok; ++i) {
        uint64_t raw = 0;
        int length = 0;
        ok = ReadVint(data, size, &pos, false, &raw, nullptr, &length);
        if (!ok) {
          break;
        }
        // Signed: the value less half its range.
        frame +=
            static_cast<int64_t>(raw) - ((int64_t{1} << (7 * length - 1)) - 1);
        ok = frame >= 0 && frame <= static_cast<int64_t>(size);
        if (ok) {
          sizes.push_back(static_cast<size_t>(frame));
          total += static_cast<size_t>(frame);
        }
      }
    }
    if (ok && lacing == 2) {
      ok = (size - pos) % count == 0;
      sizes.assign(count, (size - pos) / count);
    } else if (ok) {
      ok = total <= size - pos;
      if (ok) {
        sizes.push_back(size - pos - total);
      }
    }
    if (!ok || sizes.size() != count) {
      LOGW("track %llu: malformed lacing; block skipped",
           static_cast<unsigned long long>(number));
      return;
    }
  }

  if (track->webvtt_lines && sizes.size() == 1) {
    for (int line = 0; line < 2; ++line) {
      const uint8_t* end =
          static_cast<const uint8_t*>(memchr(data + pos, '\n', sizes[0]));
      if (!end) {
        break;
      }
      const size_t skipped = static_cast<size_t>(end - (data + pos)) + 1;
      pos += skipped;
      sizes[0] -= skipped;
    }
  }
  // Muxers write timestamps (FFmpeg's also BlockDuration) on the block
  // timeline, which is the audio's moved later by the CodecDelay rounded
  // to its ticks: undo that, so a stream ends where its last block says.
  // The first block, at 0, gets the exact delay instead, so that the
  // audio left after the decoder drops the delay starts at 0, not up to
  // half a tick off (the renderer joins the rest to it by frame count).
  const int64_t block_time = cluster_time_ + relative;
  const int64_t time =
      block_time == 0
          ? -track->codec_delay
          : ToMicros(block_time) -
                ToMicros((track->codec_delay * 1000 + timestamp_scale_ / 2) /
                         timestamp_scale_);
  int64_t frame_duration = 0;
  const bool own_duration =
      duration_ticks >= 0 && sizes.size() == 1 && track->audio;
  if (own_duration) {
    // An audio block's own duration overrides the track's default: the
    // last block is often shorter (FLAC's, and Opus's and AAC's less
    // their DiscardPadding, as FFmpeg writes it).
    frame_duration = ToMicros(duration_ticks);
  } else if (track->default_duration > 0) {
    // In whole ticks, as the timestamps are: a 33.333 ms frame at 7.467 s
    // must not look like it covers 7.500 s.
    frame_duration = ToMicros((track->default_duration + timestamp_scale_ / 2) /
                              timestamp_scale_);
  } else if (duration_ticks >= 0) {
    frame_duration =
        ToMicros(duration_ticks) / static_cast<int64_t>(sizes.size());
  }
  // DiscardPadding: audio at the block's end that is not content (Opus
  // padding up to the frame), in nanoseconds. The decoder still outputs
  // it, but the stream ends before it, so a loop's passes join where the
  // content does. A BlockDuration leaves it out already.
  if (discard_padding > 0 && !own_duration && sizes.size() == 1 &&
      frame_duration > 0) {
    frame_duration =
        std::max<int64_t>(frame_duration - discard_padding / 1000, 0);
  }
  last_time_ = std::max(
      last_time_, time + frame_duration * static_cast<int64_t>(sizes.size()));
  for (size_t i = 0; i < sizes.size(); ++i) {
    Pending pending;
    EncodedPacket& packet = pending.packet;
    packet.stream_index = track->stream;
    packet.pts = i == 0 ? time
                 : frame_duration > 0
                     ? time + static_cast<int64_t>(i) * frame_duration
                     : kNoTimestamp;
    packet.duration = frame_duration;
    packet.keyframe = keyframe;
    if (track->stripped_header.empty() && sizes.size() == 1) {
      packet.buffer = std::make_shared<BlockBuffer>(bytes, pos, sizes[0]);
    } else {
      std::vector<uint8_t> frame = track->stripped_header;
      frame.insert(frame.end(), data + pos, data + pos + sizes[i]);
      packet.buffer = PacketBuffer::Copy(frame.data(), frame.size());
    }
    pos += sizes[i];
    if (track->needs_dts && packet.pts != kNoTimestamp) {
      pending.unresolved = true;
    } else {
      packet.dts = packet.pts;
    }
    pending_.push_back(std::move(pending));
  }
}

void MatroskaDemuxer::ResolveDts(bool all) {
  for (Track& track : tracks_) {
    if (!track.needs_dts) {
      continue;
    }
    std::vector<Pending*> packets;
    for (Pending& pending : pending_) {
      if (pending.packet.stream_index == track.stream && pending.unresolved) {
        packets.push_back(&pending);
      }
    }
    // A frame decodes before every frame after it shown earlier: its dts is
    // the smallest pts from it on. Frames reorder within kDtsWindow.
    for (size_t i = 0; i < packets.size(); ++i) {
      if (!all && packets.size() - i <= kDtsWindow) {
        break;
      }
      int64_t dts = packets[i]->packet.pts;
      for (size_t j = i + 1; j < packets.size() && j <= i + kDtsWindow; ++j) {
        dts = std::min(dts, packets[j]->packet.pts);
      }
      if (track.last_dts != kNoTimestamp) {
        dts = std::max(dts, track.last_dts);
      }
      track.last_dts = dts;
      packets[i]->packet.dts = dts;
      packets[i]->unresolved = false;
    }
  }
}

void MatroskaDemuxer::ClearPending() {
  pending_.clear();
  input_done_ = false;
  for (Track& track : tracks_) {
    track.last_dts = kNoTimestamp;
    track.skip_to_keyframe = false;
  }
}

bool MatroskaDemuxer::Seek(int64_t time) {
  interrupted_ = false;
  if (!aborted_) {
    source_->ClearInterrupt();
  }
  if (!media_info_.seekable) {
    return false;
  }
  ClearPending();
  last_time_ = time;
  LoadCues();
  // Cues of the video track if it has any, else of a track being read,
  // else any.
  uint64_t cue_track = 0;
  int best = -1;
  for (const Track& track : tracks_) {
    bool has_cues = std::any_of(
        cues_.begin(), cues_.end(),
        [&](const CuePoint& cue) { return cue.track == track.number; });
    if (!has_cues) {
      continue;
    }
    int score = (track.enabled ? 2 : 0) +
                (media_info_.streams[track.stream].type == MediaType::kVideo);
    if (score > best) {
      best = score;
      cue_track = track.number;
    }
  }
  int64_t target = first_cluster_;
  if (best >= 0) {
    for (const CuePoint& cue : cues_) {
      if (cue.track == cue_track && cue.time <= time) {
        target = cue.position;
      }
    }
  } else {
    IndexClustersUpTo(time);
    for (const ClusterEntry& cluster : clusters_) {
      if (cluster.time <= time) {
        target = cluster.position;
      }
    }
  }
  // Reading the Cues or indexing was cut short: the target may be far
  // before |time|, and the caller asked for the seek to end.
  if (aborted_ || interrupted_) {
    return false;
  }
  position_ = target;
  in_cluster_ = false;
  for (Track& track : tracks_) {
    track.skip_to_keyframe = true;
  }
  return true;
}

void MatroskaDemuxer::Interrupt() {
  interrupted_ = true;
  std::lock_guard<std::mutex> lock(source_mutex_);
  if (source_) {
    source_->Interrupt();
  }
}

void MatroskaDemuxer::Abort() {
  aborted_ = true;
  std::lock_guard<std::mutex> lock(source_mutex_);
  if (source_) {
    source_->Interrupt();
  }
}

}  // namespace lychee
