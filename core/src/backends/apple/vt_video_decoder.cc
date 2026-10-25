#define LOG_TAG "vt_decoder"

#include "backends/apple/vt_video_decoder.h"

#include <TargetConditionals.h>

#include <algorithm>

#include "backends/apple/pixel_buffer.h"
#include "base/log.h"

namespace lychee {

namespace {

constexpr int kMaxConsecutiveErrors = 10;
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

}  // namespace

VTVideoDecoder::VTVideoDecoder(bool hardware) : hardware_(hardware) {}

VTVideoDecoder::~VTVideoDecoder() {
  DestroySession();
}

const char* VTVideoDecoder::name() const {
  return hardware_ ? "videotoolbox" : "videotoolbox-sw";
}

bool VTVideoDecoder::Initialize(const VideoConfig& config, std::string* error) {
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
  format_ = CreateFormat();
  if (!format_) {
    return false;
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
  int bits = 8;
  if (__builtin_available(macOS 12, iOS 15, *)) {
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
  LOGI("%s decoder opened for %s %dx%d", name(),
       converter_.hevc() ? "hevc" : "h264", size.width, size.height);
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
  if (status != noErr) {
    LOGW("%s: decode failed: %d", self->name(), static_cast<int>(status));
    if (status == kVTInvalidSessionErr ||
        status == kVTVideoDecoderMalfunctionErr) {
      self->session_lost_ = true;
    }
    self->consecutive_errors_++;
    return;
  }
  self->consecutive_errors_ = 0;
  if (!image || (flags & kVTDecodeInfo_FrameDropped)) {
    return;
  }
  CVPixelBufferRetain(image);
  self->reorder_.emplace(ToMicros(pts), Decoded{image, ToMicros(duration)});
}

void VTVideoDecoder::Emit(int64_t up_to, const OutputCallback& output) {
  while (!reorder_.empty() &&
         (reorder_.begin()->first <= up_to || reorder_.size() > kMaxReorder)) {
    auto node = reorder_.extract(reorder_.begin());
    std::shared_ptr<VideoFrame> frame = WrapPixelBuffer(node.mapped().image);
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
  status = VTDecompressionSessionDecodeFrame(session_, sample, 0, nullptr,
                                             nullptr);
  CFRelease(sample);
  if (status != noErr) {
    LOGW("%s: VTDecompressionSessionDecodeFrame: %d", name(),
         static_cast<int>(status));
    if (status == kVTInvalidSessionErr ||
        status == kVTVideoDecoderMalfunctionErr) {
      session_lost_ = true;
    }
    consecutive_errors_++;
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
}

}  // namespace lychee
