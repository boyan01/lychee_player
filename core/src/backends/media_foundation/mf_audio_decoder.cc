#define LOG_TAG "mf_audio"

#include "backends/media_foundation/mf_audio_decoder.h"

#include <mmreg.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>

#include "base/log.h"

namespace lychee {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kMaxConsecutiveErrors = 10;
// Output buffers for MFTs that do not say how large theirs should be.
constexpr DWORD kMinOutputBuffer = 64 * 1024;

}  // namespace

bool MFAudioDecoder::Initialize(const AudioConfig& config,
                                const AudioOutputFormat& output_format,
                                std::string* error) {
  output_format_ = output_format;
  // ADTS (no AudioSpecificConfig) comes only from containers this build
  // does not read.
  if (config.codec != AudioCodec::kAAC || config.extradata.empty() ||
      config.sample_rate <= 0 || config.channels <= 0) {
    *error = "mediafoundation: no decoder for " + config.codec_name;
    return false;
  }
  if (!StartMediaFoundation() ||
      FAILED(CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&decoder_)))) {
    *error = "mediafoundation: no AAC decoder";
    return false;
  }
  ComPtr<IMFMediaType> input;
  if (FAILED(MFCreateMediaType(&input))) {
    *error = "mediafoundation: MFCreateMediaType failed";
    return false;
  }
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
  input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                   static_cast<UINT32>(config.sample_rate));
  input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                   static_cast<UINT32>(config.channels));
  // Raw AAC; the user data is HEAACWAVEINFO after its WAVEFORMATEX
  // (payload type 0, profile 0xfe for "not given", the rest zero), then
  // the AudioSpecificConfig.
  input->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
  std::vector<uint8_t> user_data(12, 0);
  user_data[2] = 0xfe;
  user_data.insert(user_data.end(), config.extradata.begin(),
                   config.extradata.end());
  input->SetBlob(MF_MT_USER_DATA, user_data.data(),
                 static_cast<UINT32>(user_data.size()));
  HRESULT hr = decoder_->SetInputType(0, input.Get(), 0);
  if (FAILED(hr)) {
    *error = "mediafoundation: the AAC decoder rejects this stream: " +
             HResultString(hr);
    return false;
  }
  if (!SetUpOutput()) {
    *error = "mediafoundation: no PCM output for this stream";
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
  if (rate == output_format_.sample_rate &&
      channels == output_format_.channels && float_output_) {
    LOGI("decoder opened for aac %d Hz %d ch", rate, channels);
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
  LOGI("decoder opened for aac %d Hz %d ch -> %d Hz %d ch", rate, channels,
       output_format_.sample_rate, output_format_.channels);
  return true;
}

bool MFAudioDecoder::Decode(const EncodedPacket& packet,
                            const OutputCallback& output) {
  std::vector<ComPtr<IMFSample>> decoded;
  if (packet.end_of_stream) {
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    bool ok = Pull(decoder_.Get(), &decoded) && Deliver(&decoded, true, output);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    next_pts_ = kNoTimestamp;
    return ok;
  }
  if (!packet.buffer || packet.size() == 0) {
    return true;
  }
  if (next_pts_ == kNoTimestamp) {
    next_pts_ = packet.pts;
  }
  ComPtr<IMFSample> sample = CreateMFSample(
      packet.buffer->data(), packet.size(), packet.pts, packet.duration);
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
  if (!Pull(decoder_.Get(), &decoded)) {
    return ++consecutive_errors_ < kMaxConsecutiveErrors;
  }
  consecutive_errors_ = 0;
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
  decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  if (resampler_) {
    resampler_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
  next_pts_ = kNoTimestamp;
  consecutive_errors_ = 0;
}

}  // namespace lychee
