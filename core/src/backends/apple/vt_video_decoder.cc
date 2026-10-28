#define LOG_TAG "vt_decoder"

#include "backends/apple/vt_video_decoder.h"

#include <TargetConditionals.h>

#include <algorithm>
#include <mutex>

#include "backends/apple/pixel_buffer.h"
#include "backends/mp2t/elementary_stream.h"
#include "base/log.h"

namespace lychee {

namespace {

constexpr int kMaxConsecutiveErrors = 10;
// codecBadDataErr, what the software decoder returns for damaged data.
constexpr OSStatus kCodecBadDataErr = -8969;
// Frames held for reordering when packets carry no dts.
constexpr size_t kMaxReorder = 16;

CMTime ToCMTime(int64_t micros) {
  return micros == kNoTimestamp ? kCMTimeInvalid
                                : CMTimeMake(micros, kMicrosPerSecond);
}

int64_t ToMicros(CMTime time) {
  if (!CMTIME_IS_NUMERIC(time)) {
    return kNoTimestamp;
  }
  return CMTimeConvertScale(time, kMicrosPerSecond,
                            kCMTimeRoundingMethod_RoundHalfAwayFromZero)
      .value;
}

void SetBoolean(CFMutableDictionaryRef dictionary, CFStringRef key, bool value) {
  CFDictionarySetValue(dictionary, key, value ? kCFBooleanTrue : kCFBooleanFalse);
}

CFMutableDictionaryRef CreateDictionary() {
  return CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                   &kCFTypeDictionaryValueCallBacks);
}

// Length of the start code at |data[i]|, or 0.
size_t StartCodeAt(const std::vector<uint8_t>& data, size_t i) {
  if (i + 3 <= data.size() && data[i] == 0 && data[i + 1] == 0) {
    if (data[i + 2] == 1) {
      return 3;
    }
    if (i + 4 <= data.size() && data[i + 2] == 0 && data[i + 3] == 1) {
      return 4;
    }
  }
  return 0;
}

// VP9 is not registered with VideoToolbox until an app asks.
bool RegisterVp9() {
  if (__builtin_available(macOS 11, iOS 26.2, tvOS 26.2, *)) {
    static std::once_flag once;
    std::call_once(once, [] {
      VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
    });
    return true;
  }
  return false;
}

// Bits of a byte string, most significant first; zeros past the end.
class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  int Read(int bits) {
    int value = 0;
    for (int i = 0; i < bits; ++i, ++position_) {
      size_t byte = position_ / 8;
      int bit = byte < size_ ? (data_[byte] >> (7 - position_ % 8)) & 1 : 0;
      value = (value << 1) | bit;
    }
    return value;
  }
  bool ok() const { return position_ <= size_ * 8; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t position_ = 0;
};

void SetColor(CFMutableDictionaryRef extensions, const ColorInfo& color) {
  if (CFStringRef primaries =
          CVColorPrimariesGetStringForIntegerCodePoint(color.primaries)) {
    CFDictionarySetValue(
        extensions, kCMFormatDescriptionExtension_ColorPrimaries, primaries);
  }
  if (CFStringRef transfer =
          CVTransferFunctionGetStringForIntegerCodePoint(color.transfer)) {
    CFDictionarySetValue(
        extensions, kCMFormatDescriptionExtension_TransferFunction, transfer);
  }
  if (CFStringRef matrix =
          CVYCbCrMatrixGetStringForIntegerCodePoint(color.matrix)) {
    CFDictionarySetValue(extensions, kCMFormatDescriptionExtension_YCbCrMatrix,
                         matrix);
  }
  if (color.full_range) {
    CFDictionarySetValue(extensions,
                         kCMFormatDescriptionExtension_FullRangeVideo,
                         kCFBooleanTrue);
  }
}

}  // namespace

VTVideoDecoder::VTVideoDecoder(bool hardware) : hardware_(hardware) {}

VTVideoDecoder::~VTVideoDecoder() {
  DestroySession();
}

const char* VTVideoDecoder::name() const {
  return hardware_ ? "videotoolbox" : "videotoolbox-sw";
}

bool VTVideoDecoder::Initialize(const VideoConfig& config, std::string* error) {
  if (config.codec == VideoCodec::kVP9 || config.codec == VideoCodec::kAV1) {
    config_ = config;
    const std::vector<uint8_t>& extradata = config.extradata;
    if (config.codec == VideoCodec::kVP9) {
      if (!RegisterVp9()) {
        *error = "videotoolbox: VP9 needs iOS 26.2";
        return false;
      }
      codec_type_ = kCMVideoCodecType_VP9;
      vp9_.width = config.width;
      vp9_.height = config.height;
      // WebM's codec features (ID, length, value), if the muxer wrote them.
      for (size_t i = 0; i + 2 < extradata.size();) {
        const size_t length = extradata[i + 1];
        if (length == 1 && i + 2 < extradata.size()) {
          const int value = extradata[i + 2];
          switch (extradata[i]) {
            case 1:
              vp9_.profile = value;
              break;
            case 2:
              vp9_.level = value;
              break;
            case 3:
              vp9_.bit_depth = value;
              break;
            case 4:
              vp9_.subsampling = value == 1 ? 0 : value;
              break;
            default:
              break;
          }
        }
        i += 2 + length;
      }
      vp9_.full_range = config.color.full_range;
      bit_depth_ = vp9_.bit_depth;
    } else {
      // av1C: marker and version, profile and level, then the depth bits.
      if (extradata.size() < 4 || extradata[0] != 0x81) {
        *error = "videotoolbox: AV1 without an av1C";
        return false;
      }
      codec_type_ = kCMVideoCodecType_AV1;
      bit_depth_ = (extradata[2] & 0x20) ? 12 : (extradata[2] & 0x40) ? 10 : 8;
    }
    if (!CreateSession()) {
      *error = "videotoolbox: cannot decode this stream";
      return false;
    }
    return true;
  }
  if (!converter_.Initialize(config)) {
    *error = "videotoolbox: not H.264 or HEVC";
    return false;
  }
  config_ = config;
  parameter_sets_ = converter_.parameter_sets();
  // Without parameter sets in the extradata (MPEG-TS, live input), the
  // session waits for the first keyframe, which carries them.
  if (!parameter_sets_.empty() && !CreateSession()) {
    *error = "videotoolbox: cannot decode this stream";
    return false;
  }
  return true;
}

CMVideoFormatDescriptionRef VTVideoDecoder::CreateFormat() const {
  if (codec_type_ != 0) {
    // The sample description's configuration box, as in MP4.
    std::vector<uint8_t> box;
    int width = config_.width;
    int height = config_.height;
    if (codec_type_ == kCMVideoCodecType_VP9) {
      const ColorInfo& color = config_.color;
      box = {
          1,
          0,
          0,
          0,  // version, flags
          static_cast<uint8_t>(vp9_.profile),
          static_cast<uint8_t>(vp9_.level),
          static_cast<uint8_t>((vp9_.bit_depth << 4) | (vp9_.subsampling << 1) |
                               (vp9_.full_range ? 1 : 0)),
          color.primaries,
          color.transfer,
          color.matrix,
          0,
          0};  // codecInitializationDataSize
      width = vp9_.width;
      height = vp9_.height;
    } else {
      box = config_.extradata;
    }
    CFMutableDictionaryRef extensions = CreateDictionary();
    CFMutableDictionaryRef atoms = CreateDictionary();
    CFDataRef data =
        CFDataCreate(nullptr, box.data(), static_cast<CFIndex>(box.size()));
    CFDictionarySetValue(
        atoms,
        codec_type_ == kCMVideoCodecType_VP9 ? CFSTR("vpcC") : CFSTR("av1C"),
        data);
    CFRelease(data);
    CFDictionarySetValue(
        extensions,
        kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms);
    CFRelease(atoms);
    SetColor(extensions, config_.color);
    CMVideoFormatDescriptionRef format = nullptr;
    OSStatus status = CMVideoFormatDescriptionCreate(
        nullptr, codec_type_, width, height, extensions, &format);
    CFRelease(extensions);
    if (status != noErr) {
      LOGW("cannot make a %s format description: %d",
           config_.codec_name.c_str(), static_cast<int>(status));
      return nullptr;
    }
    return format;
  }
  std::vector<const uint8_t*> pointers;
  std::vector<size_t> sizes;
  for (const std::vector<uint8_t>& set : parameter_sets_) {
    pointers.push_back(set.data());
    sizes.push_back(set.size());
  }
  // Four-byte NAL lengths: samples are rewritten that way in Decode().
  CMVideoFormatDescriptionRef format = nullptr;
  OSStatus status =
      converter_.hevc()
          ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                nullptr, pointers.size(), pointers.data(), sizes.data(), 4,
                nullptr, &format)
          : CMVideoFormatDescriptionCreateFromH264ParameterSets(
                nullptr, pointers.size(), pointers.data(), sizes.data(), 4,
                &format);
  if (status != noErr) {
    LOGW("cannot make a format description from the parameter sets: %d",
         static_cast<int>(status));
    return nullptr;
  }
  return format;
}

bool VTVideoDecoder::CreateSession() {
  DestroySession();
  decoded_ = false;
  format_ = CreateFormat();
  if (!format_) {
    return false;
  }
  output_color_.reset();
  if (codec_type_ == 0 && !converter_.hevc()) {
    std::vector<uint8_t> headers;
    for (const auto& set : parameter_sets_) {
      AnnexBConverter::AppendNal(set.data(), set.size(), &headers);
    }
    AccessUnitInfo info;
    if (ParseAccessUnit(headers.data(), headers.size(), false, &info) &&
        info.has_sps) {
      output_color_ = config_.color;
      // Container color wins; the SPS supplies fields it left unspecified.
      if (output_color_->primaries == 2) {
        output_color_->primaries = info.color.primaries;
      }
      if (output_color_->transfer == 2) {
        output_color_->transfer = info.color.transfer;
      }
      if (output_color_->matrix == 2) {
        output_color_->matrix = info.color.matrix;
      }
    }
  }

  CFMutableDictionaryRef specification = CreateDictionary();
#if TARGET_OS_OSX
  if (hardware_) {
    SetBoolean(specification,
               kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
               true);
  } else {
    SetBoolean(specification,
               kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
               false);
  }
#endif
  // NV12, or its 10-bit form for 10-bit streams (HDR), as FFmpeg's
  // VideoToolbox hwaccel outputs them, on IOSurfaces that Metal and Core
  // Animation can show. Left to itself the decoder picks lossless
  // compressed formats (&8v0) that the CPU cannot read.
  int bits = bit_depth_;
  if (codec_type_ != 0) {
  } else if (__builtin_available(macOS 12, iOS 15, *)) {
    if (auto number = static_cast<CFNumberRef>(CMFormatDescriptionGetExtension(
            format_, kCMFormatDescriptionExtension_BitsPerComponent))) {
      CFNumberGetValue(number, kCFNumberIntType, &bits);
    }
  }
  CFMutableDictionaryRef attributes = CreateDictionary();
  int pixel_format = bits > 8 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                              : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  CFNumberRef format_number =
      CFNumberCreate(nullptr, kCFNumberIntType, &pixel_format);
  CFDictionarySetValue(attributes, kCVPixelBufferPixelFormatTypeKey,
                       format_number);
  CFRelease(format_number);
  CFMutableDictionaryRef io_surface = CreateDictionary();
  CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey,
                       io_surface);
  CFRelease(io_surface);
  SetBoolean(attributes, kCVPixelBufferMetalCompatibilityKey, true);

  VTDecompressionOutputCallbackRecord callback = {
      [](void* decoder, void* source_frame, OSStatus status,
         VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime pts,
         CMTime duration) {
        OnOutput(decoder, source_frame, status, flags, image, pts, duration);
      },
      this};
  OSStatus status = VTDecompressionSessionCreate(
      nullptr, format_, specification, attributes, &callback, &session_);
  CFRelease(specification);
  CFRelease(attributes);
  if (status != noErr) {
    LOGW("%s: VTDecompressionSessionCreate failed: %d", name(),
         static_cast<int>(status));
    session_ = nullptr;
    return false;
  }
  CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format_);
  LOGI("%s decoder opened for %s %dx%d", name(), config_.codec_name.c_str(),
       size.width, size.height);
  session_lost_ = false;
  return true;
}

void VTVideoDecoder::DestroySession() {
  if (session_) {
    VTDecompressionSessionInvalidate(session_);
    CFRelease(session_);
    session_ = nullptr;
  }
  if (format_) {
    CFRelease(format_);
    format_ = nullptr;
  }
  for (auto& [pts, decoded] : reorder_) {
    CVPixelBufferRelease(decoded.image);
  }
  reorder_.clear();
}

void VTVideoDecoder::OnOutput(void* decoder,
                              void*,
                              OSStatus status,
                              VTDecodeInfoFlags flags,
                              CVImageBufferRef image,
                              CMTime pts,
                              CMTime duration) {
  auto* self = static_cast<VTVideoDecoder*>(decoder);
  self->output_status_ = status;
  if (status != noErr) {
    return;
  }
  if (!image || (flags & kVTDecodeInfo_FrameDropped)) {
    return;
  }
  self->decoded_ = true;
  CVPixelBufferRetain(image);
  self->reorder_.emplace(ToMicros(pts), Decoded{image, ToMicros(duration)});
}

void VTVideoDecoder::Emit(int64_t up_to, const OutputCallback& output) {
  while (!reorder_.empty() &&
         (reorder_.begin()->first <= up_to || reorder_.size() > kMaxReorder)) {
    auto node = reorder_.extract(reorder_.begin());
    std::shared_ptr<VideoFrame> frame = WrapPixelBuffer(
        node.mapped().image, output_color_ ? &*output_color_ : nullptr);
    CVPixelBufferRelease(node.mapped().image);
    frame->pts = node.key();
    frame->duration = std::max<int64_t>(node.mapped().duration, 0);
    output(std::move(frame));
  }
}

bool VTVideoDecoder::Decode(const EncodedPacket& packet,
                            const OutputCallback& output) {
  if (packet.end_of_stream) {
    if (session_) {
      VTDecompressionSessionWaitForAsynchronousFrames(session_);
    }
    Emit(kInfiniteTime, output);
    return !session_lost_;
  }
  if (!packet.buffer) {
    return true;
  }
  if (await_keyframe_) {
    if (!packet.keyframe) {
      return true;
    }
    await_keyframe_ = false;
  }
  if (codec_type_ != 0) {
    // VP9 says its profile, depth and size in each keyframe's header; a
    // change needs a new session.
    Vp9Format format = vp9_;
    if (codec_type_ == kCMVideoCodecType_VP9 && packet.keyframe &&
        ParseVp9Keyframe(packet.buffer->data(), packet.buffer->size(),
                         &format) &&
        !(format == vp9_)) {
      if (session_) {
        VTDecompressionSessionWaitForAsynchronousFrames(session_);
        Emit(kInfiniteTime, output);
      }
      vp9_ = format;
      bit_depth_ = format.bit_depth;
      if (!CreateSession()) {
        return false;
      }
    }
    sample_.assign(packet.buffer->data(),
                   packet.buffer->data() + packet.buffer->size());
    return DecodeSample(packet, output);
  }

  // Rewrite the sample with four-byte NAL lengths, taking parameter sets
  // out: they belong in the format description.
  annexb_.clear();
  if (!converter_.Convert(packet.buffer->data(), packet.buffer->size(), false,
                          &annexb_)) {
    LOGW("%s: malformed sample", name());
    return ++consecutive_errors_ < kMaxConsecutiveErrors;
  }
  sample_.clear();
  std::vector<std::vector<uint8_t>> in_band;
  size_t i = 0;
  size_t start_code = 0;
  while (i < annexb_.size() && !(start_code = StartCodeAt(annexb_, i))) {
    ++i;
  }
  while (i < annexb_.size()) {
    size_t begin = i + start_code;
    size_t end = begin;
    while (end < annexb_.size() && !(start_code = StartCodeAt(annexb_, end))) {
      ++end;
    }
    i = end;
    size_t last = end;
    while (last > begin && annexb_[last - 1] == 0) {
      --last;
    }
    if (last == begin) {
      continue;
    }
    std::vector<uint8_t> nal(annexb_.begin() + begin, annexb_.begin() + last);
    int type = converter_.NalType(nal);
    if (converter_.hevc() ? type >= 32 && type <= 34 : type == 7 || type == 8) {
      in_band.push_back(std::move(nal));
      continue;
    }
    uint32_t length = static_cast<uint32_t>(nal.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
      sample_.push_back(static_cast<uint8_t>(length >> shift));
    }
    sample_.insert(sample_.end(), nal.begin(), nal.end());
  }

  if (!in_band.empty()) {
    // Each new parameter set replaces the one of its type; senders may
    // send them in separate packets.
    std::vector<std::vector<uint8_t>> sets = parameter_sets_;
    for (std::vector<uint8_t>& nal : in_band) {
      int type = converter_.NalType(nal);
      auto same = std::find_if(sets.begin(), sets.end(), [&](const auto& set) {
        return converter_.NalType(set) == type;
      });
      if (same != sets.end()) {
        *same = std::move(nal);
      } else {
        sets.push_back(std::move(nal));
      }
    }
    std::stable_sort(sets.begin(), sets.end(),
                     [this](const auto& a, const auto& b) {
                       return converter_.NalType(a) < converter_.NalType(b);
                     });
    if (sets != parameter_sets_) {
      parameter_sets_ = std::move(sets);
      CMVideoFormatDescriptionRef format = CreateFormat();
      if (format && session_ &&
          VTDecompressionSessionCanAcceptFormatDescription(session_, format)) {
        CFRelease(format_);
        format_ = format;
      } else if (format) {
        // A new resolution, or the first parameter sets of a stream that
        // carries them in band: frames decoded so far go out, then a new
        // session decodes the rest.
        CFRelease(format);
        if (session_) {
          VTDecompressionSessionWaitForAsynchronousFrames(session_);
          Emit(kInfiniteTime, output);
        }
        if (!CreateSession()) {
          return false;
        }
      }
      // Without a format the sets are incomplete; the rest follow.
    }
  }
  return DecodeSample(packet, output);
}

// static
bool VTVideoDecoder::ParseVp9Keyframe(const uint8_t* data,
                                      size_t size,
                                      Vp9Format* format) {
  // The uncompressed header (VP9 bitstream specification, 6.2).
  BitReader bits(data, size);
  if (bits.Read(2) != 2) {  // frame_marker
    return false;
  }
  int profile = bits.Read(1);
  profile |= bits.Read(1) << 1;
  if (profile == 3) {
    bits.Read(1);
  }
  if (bits.Read(1)) {  // show_existing_frame
    return false;
  }
  if (bits.Read(1) != 0) {  // frame_type: not a key frame
    return false;
  }
  bits.Read(2);                     // show_frame, error_resilient_mode
  if (bits.Read(24) != 0x498342) {  // frame_sync_code
    return false;
  }
  int bit_depth = 8;
  if (profile >= 2) {
    bit_depth = bits.Read(1) ? 12 : 10;
  }
  int subsampling = 0;
  bool full_range = true;
  if (bits.Read(3) != 7) {  // color_space other than RGB
    full_range = bits.Read(1);
    if (profile == 1 || profile == 3) {
      const int x = bits.Read(1);
      const int y = bits.Read(1);
      bits.Read(1);
      subsampling = x && y ? 0 : x ? 2 : 3;
    }
  } else {
    subsampling = 3;
    if (profile == 1 || profile == 3) {
      bits.Read(1);
    }
  }
  const int width = bits.Read(16) + 1;
  const int height = bits.Read(16) + 1;
  if (!bits.ok()) {
    return false;
  }
  format->profile = profile;
  format->bit_depth = bit_depth;
  format->subsampling = subsampling;
  format->full_range = full_range;
  format->width = width;
  format->height = height;
  return true;
}

bool VTVideoDecoder::DecodeSample(const EncodedPacket& packet,
                                  const OutputCallback& output) {
  if (!session_ || sample_.empty()) {
    return true;
  }

  CMBlockBufferRef block = nullptr;
  OSStatus status = CMBlockBufferCreateWithMemoryBlock(
      nullptr, nullptr, sample_.size(), nullptr, nullptr, 0, sample_.size(),
      kCMBlockBufferAssureMemoryNowFlag, &block);
  if (status == noErr) {
    status = CMBlockBufferReplaceDataBytes(sample_.data(), block, 0,
                                           sample_.size());
  }
  CMSampleBufferRef sample = nullptr;
  if (status == noErr) {
    CMSampleTimingInfo timing = {ToCMTime(packet.duration > 0 ? packet.duration
                                                              : kNoTimestamp),
                                 ToCMTime(packet.pts), ToCMTime(packet.dts)};
    size_t size = sample_.size();
    status = CMSampleBufferCreateReady(nullptr, block, format_, 1, 1, &timing,
                                       1, &size, &sample);
  }
  if (block) {
    CFRelease(block);
  }
  if (status != noErr) {
    LOGE("%s: cannot make a sample buffer: %d", name(),
         static_cast<int>(status));
    return false;
  }
  // Synchronous: the output callback runs before this returns.
  output_status_ = noErr;
  status = VTDecompressionSessionDecodeFrame(session_, sample, 0, nullptr,
                                             nullptr);
  CFRelease(sample);
  if (status == noErr) {
    status = output_status_;
  }
  if (status == noErr) {
    consecutive_errors_ = 0;
  } else {
    LOGW("%s: decode failed: %d", name(), static_cast<int>(status));
    if (status == kVTInvalidSessionErr ||
        status == kVTVideoDecoderMalfunctionErr) {
      session_lost_ = true;
    }
    // After damaged data (lost TS packets) VideoToolbox fails every
    // frame that refers to it, where FFmpeg conceals: decode on from the
    // next keyframe, unless no frame came out yet (a stream it cannot
    // decode) or keyframes keep failing.
    const bool damaged = decoded_ && (status == kVTVideoDecoderBadDataErr ||
                                      status == kCodecBadDataErr);
    if (damaged) {
      await_keyframe_ = true;
    }
    if (!damaged || packet.keyframe) {
      consecutive_errors_++;
    }
  }
  if (session_lost_ || consecutive_errors_ >= kMaxConsecutiveErrors) {
    return false;
  }

  // A frame can go out once no frame decoded later can come before it:
  // every later packet has a dts at or above this one's, and a pts at or
  // above its dts.
  if (config_.low_delay) {
    Emit(kInfiniteTime, output);
  } else {
    Emit(packet.dts == kNoTimestamp ? kNoTimestamp : packet.dts, output);
  }
  return true;
}

void VTVideoDecoder::Flush() {
  if (session_) {
    VTDecompressionSessionWaitForAsynchronousFrames(session_);
  }
  for (auto& [pts, decoded] : reorder_) {
    CVPixelBufferRelease(decoded.image);
  }
  reorder_.clear();
  consecutive_errors_ = 0;
  await_keyframe_ = false;
}

}  // namespace lychee
