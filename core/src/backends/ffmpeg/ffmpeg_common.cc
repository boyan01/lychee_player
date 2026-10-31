#include "backends/ffmpeg/ffmpeg_common.h"

#include <cstring>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace lychee {

FFmpegCodecParams::FFmpegCodecParams(const AVCodecParameters* source)
    : parameters(avcodec_parameters_alloc()) {
  avcodec_parameters_copy(parameters, source);
}

FFmpegCodecParams::~FFmpegCodecParams() {
  avcodec_parameters_free(&parameters);
}

std::string AVErrorString(int error) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(error, buffer, sizeof(buffer));
  return buffer;
}

VideoCodec VideoCodecFromAVCodecID(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_H264:
      return VideoCodec::kH264;
    case AV_CODEC_ID_HEVC:
      return VideoCodec::kHEVC;
    case AV_CODEC_ID_VP8:
      return VideoCodec::kVP8;
    case AV_CODEC_ID_VP9:
      return VideoCodec::kVP9;
    case AV_CODEC_ID_AV1:
      return VideoCodec::kAV1;
    case AV_CODEC_ID_MPEG2VIDEO:
      return VideoCodec::kMPEG2;
    case AV_CODEC_ID_MPEG4:
      return VideoCodec::kMPEG4;
    case AV_CODEC_ID_NONE:
      return VideoCodec::kUnknown;
    default:
      return VideoCodec::kOther;
  }
}

AudioCodec AudioCodecFromAVCodecID(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_AAC:
      return AudioCodec::kAAC;
    case AV_CODEC_ID_MP3:
      return AudioCodec::kMP3;
    case AV_CODEC_ID_OPUS:
      return AudioCodec::kOpus;
    case AV_CODEC_ID_VORBIS:
      return AudioCodec::kVorbis;
    case AV_CODEC_ID_FLAC:
      return AudioCodec::kFLAC;
    case AV_CODEC_ID_ALAC:
      return AudioCodec::kALAC;
    case AV_CODEC_ID_AC3:
      return AudioCodec::kAC3;
    case AV_CODEC_ID_EAC3:
      return AudioCodec::kEAC3;
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_F32LE:
      return AudioCodec::kPCM;
    case AV_CODEC_ID_NONE:
      return AudioCodec::kUnknown;
    default:
      return AudioCodec::kOther;
  }
}

AVCodecID AVCodecIDFromVideoCodec(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::kH264:
      return AV_CODEC_ID_H264;
    case VideoCodec::kHEVC:
      return AV_CODEC_ID_HEVC;
    case VideoCodec::kVP8:
      return AV_CODEC_ID_VP8;
    case VideoCodec::kVP9:
      return AV_CODEC_ID_VP9;
    case VideoCodec::kAV1:
      return AV_CODEC_ID_AV1;
    case VideoCodec::kMPEG2:
      return AV_CODEC_ID_MPEG2VIDEO;
    case VideoCodec::kMPEG4:
      return AV_CODEC_ID_MPEG4;
    case VideoCodec::kOther:
    case VideoCodec::kUnknown:
      break;
  }
  return AV_CODEC_ID_NONE;
}

AVCodecID AVCodecIDFromAudioCodec(AudioCodec codec) {
  switch (codec) {
    case AudioCodec::kAAC:
      return AV_CODEC_ID_AAC;
    case AudioCodec::kMP3:
      return AV_CODEC_ID_MP3;
    case AudioCodec::kOpus:
      return AV_CODEC_ID_OPUS;
    case AudioCodec::kVorbis:
      return AV_CODEC_ID_VORBIS;
    case AudioCodec::kFLAC:
      return AV_CODEC_ID_FLAC;
    case AudioCodec::kALAC:
      return AV_CODEC_ID_ALAC;
    case AudioCodec::kAC3:
      return AV_CODEC_ID_AC3;
    case AudioCodec::kEAC3:
      return AV_CODEC_ID_EAC3;
    case AudioCodec::kPCM:
    case AudioCodec::kOther:
    case AudioCodec::kUnknown:
      break;
  }
  return AV_CODEC_ID_NONE;
}

bool FFmpegDecodes(VideoCodec codec) {
  const AVCodecID id = AVCodecIDFromVideoCodec(codec);
  return id != AV_CODEC_ID_NONE && avcodec_find_decoder(id) != nullptr;
}

bool FFmpegDecodes(AudioCodec codec) {
  const AVCodecID id = AVCodecIDFromAudioCodec(codec);
  return id != AV_CODEC_ID_NONE && avcodec_find_decoder(id) != nullptr;
}

namespace {

void CopyExtradata(const std::vector<uint8_t>& extradata,
                   AVCodecParameters* parameters) {
  if (extradata.empty()) {
    return;
  }
  parameters->extradata = static_cast<uint8_t*>(
      av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  memcpy(parameters->extradata, extradata.data(), extradata.size());
  parameters->extradata_size = static_cast<int>(extradata.size());
}

const AVCodecParameters* AttachedParameters(
    const std::shared_ptr<const CodecPrivate>& codec_private) {
  auto* params = dynamic_cast<const FFmpegCodecParams*>(codec_private.get());
  return params ? params->parameters : nullptr;
}

}  // namespace

AVCodecParameters* CreateCodecParameters(const VideoConfig& config) {
  AVCodecParameters* parameters = avcodec_parameters_alloc();
  if (const AVCodecParameters* attached =
          AttachedParameters(config.codec_private)) {
    avcodec_parameters_copy(parameters, attached);
    return parameters;
  }
  parameters->codec_type = AVMEDIA_TYPE_VIDEO;
  parameters->codec_id = AVCodecIDFromVideoCodec(config.codec);
  parameters->width = config.width;
  parameters->height = config.height;
  CopyExtradata(config.extradata, parameters);
  return parameters;
}

AVCodecParameters* CreateCodecParameters(const AudioConfig& config) {
  AVCodecParameters* parameters = avcodec_parameters_alloc();
  if (const AVCodecParameters* attached =
          AttachedParameters(config.codec_private)) {
    avcodec_parameters_copy(parameters, attached);
    return parameters;
  }
  parameters->codec_type = AVMEDIA_TYPE_AUDIO;
  parameters->codec_id = AVCodecIDFromAudioCodec(config.codec);
  parameters->sample_rate = config.sample_rate;
  av_channel_layout_default(&parameters->ch_layout, config.channels);
  CopyExtradata(config.extradata, parameters);
  return parameters;
}

AVCodecParameters* CreateCodecParameters(const SubtitleConfig& config) {
  AVCodecParameters* parameters = avcodec_parameters_alloc();
  if (const AVCodecParameters* attached =
          AttachedParameters(config.codec_private)) {
    avcodec_parameters_copy(parameters, attached);
    return parameters;
  }
  parameters->codec_type = AVMEDIA_TYPE_SUBTITLE;
  if (const AVCodecDescriptor* descriptor =
          avcodec_descriptor_get_by_name(config.codec_name.c_str())) {
    parameters->codec_id = descriptor->id;
  }
  CopyExtradata(config.extradata, parameters);
  return parameters;
}

static_assert(kPacketPadding >= AV_INPUT_BUFFER_PADDING_SIZE,
              "decoders need FFmpeg's padding after packet data");

std::shared_ptr<PacketBuffer> TakePacketBuffer(AVPacket* av_packet) {
  class AVPacketBuffer : public PacketBuffer {
   public:
    explicit AVPacketBuffer(AVPacket* packet) : packet_(packet) {
      data_ = packet->data;
      size_ = static_cast<size_t>(packet->size);
    }
    ~AVPacketBuffer() override { av_packet_free(&packet_); }

   private:
    AVPacket* packet_;
  };
  // Demuxers return reference-counted, padded packets; this only copies if
  // one did not.
  if (av_packet_make_refcounted(av_packet) < 0) {
    av_packet_unref(av_packet);
    return nullptr;
  }
  AVPacket* owned = av_packet_alloc();
  if (!owned) {
    av_packet_unref(av_packet);
    return nullptr;
  }
  av_packet_move_ref(owned, av_packet);
  return std::make_shared<AVPacketBuffer>(owned);
}

int WrapInAVPacket(const EncodedPacket& packet, AVPacket* av_packet) {
  const PacketBuffer& buffer = *packet.buffer;
  auto* reference = new std::shared_ptr<const PacketBuffer>(packet.buffer);
  AVBufferRef* ref = av_buffer_create(
      const_cast<uint8_t*>(buffer.data()), buffer.size() + kPacketPadding,
      [](void* opaque, uint8_t*) {
        delete static_cast<std::shared_ptr<const PacketBuffer>*>(opaque);
      },
      reference, AV_BUFFER_FLAG_READONLY);
  if (!ref) {
    delete reference;
    return AVERROR(ENOMEM);
  }
  av_packet->buf = ref;
  av_packet->data = ref->data;
  av_packet->size = static_cast<int>(buffer.size());
  av_packet->pts = packet.pts == kNoTimestamp ? AV_NOPTS_VALUE : packet.pts;
  av_packet->dts = packet.dts == kNoTimestamp ? AV_NOPTS_VALUE : packet.dts;
  av_packet->duration = packet.duration;
  av_packet->flags = packet.keyframe ? AV_PKT_FLAG_KEY : 0;
  return 0;
}

}  // namespace lychee
