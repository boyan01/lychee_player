#define LOG_TAG "mf_audio"

#include "backends/media_foundation/mf_audio_decoder.h"

#include <mmreg.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>

#include "base/log.h"
#include "base/time.h"

namespace lychee {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kMaxConsecutiveErrors = 10;
// Output buffers for MFTs that do not say how large theirs should be.
constexpr DWORD kMinOutputBuffer = 64 * 1024;

// The size of the ADTS header |data| starts with, or 0.
size_t AdtsHeaderSize(const uint8_t* data, size_t size) {
  if (size < 7 || data[0] != 0xff || (data[1] & 0xf6) != 0xf0) {
    return 0;
  }
  // Nine bytes with a CRC (protection_absent clear).
  const size_t header = (data[1] & 1) ? 7 : 9;
  return header < size ? header : 0;
}

// The input subtype for |codec|, or GUID_NULL.
GUID Subtype(AudioCodec codec) {
  switch (codec) {
    case AudioCodec::kAAC:
      return MFAudioFormat_AAC;
    case AudioCodec::kMP3:
      return MFAudioFormat_MP3;
    case AudioCodec::kFLAC:
      return MFAudioFormat_FLAC;
    case AudioCodec::kALAC:
      return MFAudioFormat_ALAC;
    case AudioCodec::kOpus:
      return MFAudioFormat_Opus;
    default:
      return GUID_NULL;
  }
}

// FLAC's STREAMINFO from the container's configuration: the bare block
// (MP4's dfLa without its header, as FFmpeg has it), or "fLaC" and the
// metadata blocks (Matroska's CodecPrivate). Empty if none.
std::vector<uint8_t> FlacStreamInfo(const std::vector<uint8_t>& config) {
  constexpr size_t kStreamInfo = 34;
  if (config.size() == kStreamInfo) {
    return config;
  }
  if (config.size() >= 8 + kStreamInfo &&
      std::memcmp(config.data(), "fLaC", 4) == 0 && (config[4] & 0x7f) == 0) {
    return {config.begin() + 8, config.begin() + 8 + kStreamInfo};
  }
  return {};
}

// What the decoder takes as MF_MT_USER_DATA, as Windows' own MP4 source
// hands it over: for FLAC the STREAMINFO block with its header, marked
// last; for ALAC the 24-byte ALACSpecificConfig (MP4's alac box comes with
// its header, version and flags); for Opus the OpusHead packet.
std::vector<uint8_t> UserData(AudioCodec codec,
                              const std::vector<uint8_t>& config) {
  switch (codec) {
    case AudioCodec::kFLAC: {
      const std::vector<uint8_t> info = FlacStreamInfo(config);
      if (info.empty()) {
        return {};
      }
      std::vector<uint8_t> block = {0x80, 0, 0,
                                    static_cast<uint8_t>(info.size())};
      block.insert(block.end(), info.begin(), info.end());
      return block;
    }
    case AudioCodec::kALAC:
      if (config.size() >= 12 + 24 && std::memcmp(&config[4], "alac", 4) == 0) {
        return {config.begin() + 12, config.end()};
      }
      return config;
    default:
      return config;
  }
}

// Samples at 48 kHz in an Opus packet, from its TOC byte (RFC 6716, 3.1).
// Multistream packets start with the first stream's, of the same length.
int OpusPacketFrames(const uint8_t* data, size_t size) {
  if (size < 1) {
    return 0;
  }
  static const int kSilk[] = {480, 960, 1920, 2880};
  static const int kCelt[] = {120, 240, 480, 960};
  const int config = data[0] >> 3;
  const int frame = config < 12   ? kSilk[config & 3]
                    : config < 16 ? ((config & 1) ? 960 : 480)
                                  : kCelt[config & 3];
  switch (data[0] & 3) {
    case 0:
      return frame;
    case 1:
    case 2:
      return 2 * frame;
    default:
      return size > 1 ? frame * (data[1] & 0x3f) : 0;
  }
}

ComPtr<IMFTransform> CreateDecoder(const GUID& subtype) {
  ComPtr<IMFTransform> decoder;
  MFT_REGISTER_TYPE_INFO input = {MFMediaType_Audio, subtype};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER,
                       MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                           MFT_ENUM_FLAG_SORTANDFILTER,
                       &input, nullptr, &activates, &count))) {
    return nullptr;
  }
  for (UINT32 i = 0; i < count; ++i) {
    if (!decoder) {
      activates[i]->ActivateObject(IID_PPV_ARGS(&decoder));
    }
    activates[i]->Release();
  }
  CoTaskMemFree(activates);
  return decoder;
}

}  // namespace

bool MFAudioDecoder::Initialize(const AudioConfig& config,
                                const AudioOutputFormat& output_format,
                                std::string* error) {
  output_format_ = output_format;
  config_ = config;
  if (Subtype(config.codec) == GUID_NULL || !StartMediaFoundation()) {
    *error = "mediafoundation: no decoder for " + config.codec_name;
    return false;
  }
  // AAC in ADTS (MPEG-TS) has no AudioSpecificConfig: the decoder is made
  // from the first packet's header.
  if (config.codec == AudioCodec::kAAC && config.extradata.empty()) {
    return true;
  }
  if ((config.sample_rate <= 0 || config.channels <= 0) ||
      !Open(config.extradata, error)) {
    if (error->empty()) {
      *error = "mediafoundation: no decoder for " + config.codec_name;
    }
    return false;
  }
  return true;
}

bool MFAudioDecoder::Open(const std::vector<uint8_t>& extradata,
                          std::string* error) {
  const bool aac = config_.codec == AudioCodec::kAAC;
  const GUID subtype = Subtype(config_.codec);
  if (aac) {
    CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&decoder_));
  } else {
    // MP3, and Windows' own FLAC, ALAC and Opus decoders (Windows 10).
    decoder_ = CreateDecoder(subtype);
  }
  if (!decoder_) {
    *error =
        std::string("mediafoundation: no ") + config_.codec_name + " decoder";
    return false;
  }
  ComPtr<IMFMediaType> input;
  if (FAILED(MFCreateMediaType(&input))) {
    *error = "mediafoundation: MFCreateMediaType failed";
    return false;
  }
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  input->SetGUID(MF_MT_SUBTYPE, subtype);
  input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                   static_cast<UINT32>(config_.sample_rate));
  input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                   static_cast<UINT32>(config_.channels));
  if (aac) {
    // Raw AAC; the user data is HEAACWAVEINFO after its WAVEFORMATEX
    // (payload type 0, profile 0xfe for "not given", the rest zero), then
    // the AudioSpecificConfig.
    input->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    std::vector<uint8_t> user_data(12, 0);
    user_data[2] = 0xfe;
    user_data.insert(user_data.end(), extradata.begin(), extradata.end());
    input->SetBlob(MF_MT_USER_DATA, user_data.data(),
                   static_cast<UINT32>(user_data.size()));
  } else if (config_.codec != AudioCodec::kMP3) {
    const std::vector<uint8_t> user_data = UserData(config_.codec, extradata);
    if (config_.codec == AudioCodec::kFLAC && user_data.empty()) {
      *error = "mediafoundation: FLAC without its STREAMINFO";
      decoder_.Reset();
      return false;
    }
    if (!user_data.empty()) {
      input->SetBlob(MF_MT_USER_DATA, user_data.data(),
                     static_cast<UINT32>(user_data.size()));
    }
    if (config_.codec == AudioCodec::kALAC && user_data.size() >= 4 &&
        config_.sample_rate > 0) {
      // The ALACSpecificConfig starts with the samples per packet.
      const int64_t frames = (int64_t{user_data[0]} << 24) |
                             (user_data[1] << 16) | (user_data[2] << 8) |
                             user_data[3];
      default_duration_ = frames * kMicrosPerSecond / config_.sample_rate;
    }
  }
  HRESULT hr = decoder_->SetInputType(0, input.Get(), 0);
  if (FAILED(hr)) {
    *error = "mediafoundation: the decoder rejects this stream: " +
             HResultString(hr);
    decoder_.Reset();
    return false;
  }
  if (!SetUpOutput()) {
    *error = "mediafoundation: no PCM output for this stream";
    decoder_.Reset();
    return false;
  }
  decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  return true;
}

bool MFAudioDecoder::SetUpOutput() {
  // Float if offered, else 16-bit integers.
  ComPtr<IMFMediaType> chosen;
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> type;
    if (FAILED(decoder_->GetOutputAvailableType(0, i, &type))) {
      break;
    }
    GUID subtype = {};
    type->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (subtype == MFAudioFormat_Float) {
      chosen = type;
      break;
    }
    if (subtype == MFAudioFormat_PCM && !chosen &&
        MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0) ==
            16) {
      chosen = type;
    }
  }
  if (!chosen || FAILED(decoder_->SetOutputType(0, chosen.Get(), 0))) {
    return false;
  }
  GUID subtype = {};
  chosen->GetGUID(MF_MT_SUBTYPE, &subtype);
  const int rate = static_cast<int>(
      MFGetAttributeUINT32(chosen.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
  const int channels = static_cast<int>(
      MFGetAttributeUINT32(chosen.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
  resampler_.Reset();
  float_output_ = subtype == MFAudioFormat_Float;
  decoder_rate_ = rate;
  decoder_frame_bytes_ = (float_output_ ? 4 : 2) * channels;
  const char* codec = config_.codec_name.c_str();
  if (rate == output_format_.sample_rate &&
      channels == output_format_.channels && float_output_) {
    LOGI("decoder opened for %s %d Hz %d ch", codec, rate, channels);
    return true;
  }

  ComPtr<IMFMediaType> output;
  if (FAILED(MFCreateMediaType(&output))) {
    return false;
  }
  const UINT32 out_channels = static_cast<UINT32>(output_format_.channels);
  const UINT32 out_rate = static_cast<UINT32>(output_format_.sample_rate);
  output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
  output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, out_channels);
  output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, out_rate);
  output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
  output->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4 * out_channels);
  output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                    4 * out_channels * out_rate);
  output->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  if (out_channels <= 2) {
    // The resampler mixes between layouts by their channel masks.
    output->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK,
                      out_channels == 1
                          ? SPEAKER_FRONT_CENTER
                          : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
  }
  HRESULT hr;
  if (FAILED(hr = CoCreateInstance(CLSID_CResamplerMediaObject, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&resampler_))) ||
      FAILED(hr = resampler_->SetInputType(0, chosen.Get(), 0)) ||
      FAILED(hr = resampler_->SetOutputType(0, output.Get(), 0))) {
    LOGE("no resampler from %d Hz %d ch to %d Hz %d ch: %s", rate, channels,
         output_format_.sample_rate, output_format_.channels,
         HResultString(hr).c_str());
    resampler_.Reset();
    return false;
  }
  resampler_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  resampler_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  float_output_ = true;
  LOGI("decoder opened for %s %d Hz %d ch -> %d Hz %d ch", codec, rate,
       channels, output_format_.sample_rate, output_format_.channels);
  return true;
}

bool MFAudioDecoder::Decode(const EncodedPacket& packet,
                            const OutputCallback& output) {
  std::vector<ComPtr<IMFSample>> decoded;
  if (packet.end_of_stream && !decoder_) {
    return true;
  }
  if (packet.end_of_stream) {
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    bool ok = Pull(decoder_.Get(), &decoded) && Deliver(&decoded, true, output);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    next_pts_ = kNoTimestamp;
    expected_us_ = 0;
    return ok;
  }
  if (!packet.buffer || packet.size() == 0) {
    return true;
  }
  const uint8_t* data = packet.buffer->data();
  size_t size = packet.size();
  const size_t adts =
      config_.codec == AudioCodec::kAAC && config_.extradata.empty()
          ? AdtsHeaderSize(data, size)
          : 0;
  if (!decoder_) {
    // ADTS: the AudioSpecificConfig from the header's object type,
    // sampling frequency index and channel configuration.
    if (adts == 0) {
      LOGE("AAC without a configuration or ADTS headers");
      return false;
    }
    const int object_type = (data[2] >> 6) + 1;
    const int frequency = (data[2] >> 2) & 0xf;
    const int channels = ((data[2] & 1) << 2) | (data[3] >> 6);
    const std::vector<uint8_t> asc = {
        static_cast<uint8_t>((object_type << 3) | (frequency >> 1)),
        static_cast<uint8_t>(((frequency & 1) << 7) | (channels << 3))};
    std::string error;
    if (!Open(asc, &error)) {
      LOGE("%s", error.c_str());
      return false;
    }
  }
  data += adts;
  size -= adts;
  if (next_pts_ == kNoTimestamp) {
    next_pts_ = packet.pts;
  }
  ComPtr<IMFSample> sample =
      CreateMFSample(data, size, packet.pts,
                     packet.duration > 0 ? packet.duration : default_duration_);
  if (!sample) {
    return false;
  }
  HRESULT hr = decoder_->ProcessInput(0, sample.Get(), 0);
  if (hr == MF_E_NOTACCEPTING) {
    if (!Pull(decoder_.Get(), &decoded)) {
      return false;
    }
    hr = decoder_->ProcessInput(0, sample.Get(), 0);
  }
  if (FAILED(hr)) {
    LOGW("ProcessInput: %s", HResultString(hr).c_str());
    return ++consecutive_errors_ < kMaxConsecutiveErrors;
  }
  // Microseconds of audio in the packet, for the codecs whose decoders
  // drop their delay.
  int64_t packet_us = 0;
  if (config_.codec == AudioCodec::kMP3 && config_.sample_rate > 0) {
    packet_us = (config_.sample_rate < 32000 ? 576 : 1152) * kMicrosPerSecond /
                config_.sample_rate;
  } else if (config_.codec == AudioCodec::kOpus) {
    packet_us = OpusPacketFrames(data, size) * kMicrosPerSecond / 48000;
  }
  if (!Pull(decoder_.Get(), &decoded)) {
    return ++consecutive_errors_ < kMaxConsecutiveErrors;
  }
  consecutive_errors_ = 0;
  if (expected_us_ >= 0 && packet_us > 0) {
    expected_us_ += packet_us;
    int64_t decoded_bytes = 0;
    for (ComPtr<IMFSample>& out : decoded) {
      DWORD length = 0;
      if (SUCCEEDED(out->GetTotalLength(&length))) {
        decoded_bytes += length;
      }
    }
    if (decoded_bytes > 0 && decoder_frame_bytes_ > 0 && decoder_rate_ > 0) {
      // Media Foundation's decoders drop the decoder's delay (529 samples
      // for MP3, the pre-skip for Opus) from the start, which the
      // container's timestamps count: the first output is later than the
      // packets by what it lacks, if less than a packet.
      const int64_t decoded_us = decoded_bytes / decoder_frame_bytes_ *
                                 kMicrosPerSecond / decoder_rate_;
      const int64_t missing = expected_us_ - decoded_us;
      if (missing > 0 && missing < packet_us && next_pts_ != kNoTimestamp) {
        next_pts_ += missing;
      }
      expected_us_ = -1;
    }
  }
  return Deliver(&decoded, false, output);
}

bool MFAudioDecoder::Pull(IMFTransform* mft,
                          std::vector<ComPtr<IMFSample>>* samples) {
  for (;;) {
    MFT_OUTPUT_STREAM_INFO info = {};
    if (FAILED(mft->GetOutputStreamInfo(0, &info))) {
      return false;
    }
    const bool provides =
        info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                        MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
    ComPtr<IMFSample> sample;
    if (!provides) {
      sample = CreateMFOutputSample(std::max(info.cbSize, kMinOutputBuffer));
      if (!sample) {
        return false;
      }
    }
    MFT_OUTPUT_DATA_BUFFER buffer = {};
    buffer.pSample = sample.Get();
    DWORD status = 0;
    HRESULT hr = mft->ProcessOutput(0, 1, &buffer, &status);
    if (buffer.pEvents) {
      buffer.pEvents->Release();
    }
    if (provides && buffer.pSample) {
      sample.Attach(buffer.pSample);
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      return true;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE && mft == decoder_.Get()) {
      // A new format (HE-AAC found in the stream), which the decoder
      // announces before its first output in it.
      if (!SetUpOutput()) {
        return false;
      }
      continue;
    }
    if (FAILED(hr)) {
      LOGW("ProcessOutput: %s", HResultString(hr).c_str());
      return false;
    }
    if (sample) {
      samples->push_back(std::move(sample));
    }
  }
}

bool MFAudioDecoder::Deliver(std::vector<ComPtr<IMFSample>>* decoded,
                             bool drain,
                             const OutputCallback& output) {
  if (!resampler_) {
    for (ComPtr<IMFSample>& sample : *decoded) {
      Emit(sample.Get(), output);
    }
    return true;
  }
  std::vector<ComPtr<IMFSample>> resampled;
  for (ComPtr<IMFSample>& sample : *decoded) {
    HRESULT hr = resampler_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      if (!Pull(resampler_.Get(), &resampled)) {
        return false;
      }
      hr = resampler_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr) || !Pull(resampler_.Get(), &resampled)) {
      LOGW("resampler: %s", HResultString(hr).c_str());
      return false;
    }
  }
  if (drain) {
    resampler_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (!Pull(resampler_.Get(), &resampled)) {
      return false;
    }
  }
  for (ComPtr<IMFSample>& sample : resampled) {
    Emit(sample.Get(), output);
  }
  return true;
}

void MFAudioDecoder::Emit(IMFSample* sample, const OutputCallback& output) {
  ComPtr<IMFMediaBuffer> buffer;
  BYTE* data = nullptr;
  DWORD length = 0;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) ||
      FAILED(buffer->Lock(&data, nullptr, &length))) {
    return;
  }
  auto audio = std::make_shared<AudioBuffer>();
  audio->sample_rate = output_format_.sample_rate;
  audio->channels = output_format_.channels;
  const size_t bytes = float_output_ ? sizeof(float) : sizeof(int16_t);
  const size_t frame_bytes = bytes * output_format_.channels;
  const size_t count = length / frame_bytes * output_format_.channels;
  audio->samples.resize(count);
  if (float_output_) {
    std::memcpy(audio->samples.data(), data, count * sizeof(float));
  } else {
    for (size_t i = 0; i < count; ++i) {
      int16_t value;
      std::memcpy(&value, data + i * sizeof(int16_t), sizeof(value));
      audio->samples[i] = value / 32768.0f;
    }
  }
  buffer->Unlock();
  if (audio->samples.empty()) {
    return;
  }
  audio->pts = next_pts_;
  if (next_pts_ != kNoTimestamp) {
    next_pts_ += audio->duration();
  }
  output(std::move(audio));
}

void MFAudioDecoder::Flush() {
  if (decoder_) {
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
  if (resampler_) {
    resampler_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
  next_pts_ = kNoTimestamp;
  expected_us_ = 0;
  consecutive_errors_ = 0;
}

}  // namespace lychee
