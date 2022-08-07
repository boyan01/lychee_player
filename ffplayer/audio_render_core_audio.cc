//
// Created by Bin Yang on 2022/8/7.
//
#ifdef LYCHEE_OSX

#include "audio_render_core_audio.h"
#include <cmath>

#include "logging.h"

namespace {

const int kAudioMinBufferSize = 512;
const int kAudioMaxCallbacksPerSec = 30;

AudioChannelLayoutTag GetCoreAudioChannelLayoutTag(int channels) {
  switch (channels) {
    case 1:
      return kAudioChannelLayoutTag_Mono;
    case 2:
      return kAudioChannelLayoutTag_Stereo;
    case 3:
      return kAudioChannelLayoutTag_DVD_4;
    case 4:
      return kAudioChannelLayoutTag_Quadraphonic;
    case 5:
      return kAudioChannelLayoutTag_MPEG_5_0_A;
    case 6:
      return kAudioChannelLayoutTag_MPEG_5_1_A;
    case 7:
      /* FIXME: Need to move channel[4] (BC) to channel[6] */
      return kAudioChannelLayoutTag_MPEG_6_1_A;
    case 8:
      return kAudioChannelLayoutTag_MPEG_7_1_A;
    default:
      return 0;
  }
  NOTREACHED();
}

}  // namespace

int AudioRenderCoreAudio::OpenAudioDevice(int64_t wanted_channel_layout,
                                          int wanted_nb_channels,
                                          int wanted_sample_rate,
                                          AudioParams& device_output) {
  AudioStreamBasicDescription description{};
  description.mSampleRate = wanted_sample_rate;
  description.mFormatID = kAudioFormatLinearPCM;
  /**
   * https://www.jianshu.com/p/be78830eefbd
   *
   * AV_SAMPLE_FMT_S16   kAudioFormatFlagIsSignedInteger |
   * kAudioFormatFlagIsPacked AV_SAMPLE_FMT_S16P kAudioFormatFlagIsSignedInteger
   * | kAudioFormatFlagIsNonInterleaved AV_SAMPLE_FMT_FLT
   * kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked AV_SAMPLE_FMT_FLTP
   * kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved
   */
  description.mFormatFlags =
      kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  description.mBytesPerPacket = 4;
  description.mFramesPerPacket = 1;
  description.mBytesPerFrame = 4;
  description.mChannelsPerFrame = 2;
  description.mBitsPerChannel = 16;
  auto ret = AudioQueueNewOutput(
      &description,
      [](void* in_user_data, AudioQueueRef audio_queue,
         AudioQueueBufferRef buffer) {
        auto* sink = static_cast<AudioRenderCoreAudio*>(in_user_data);
        sink->ReadAudioData((uint8*)buffer->mAudioData,
                            (int)buffer->mAudioDataBytesCapacity);
        buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
        AudioQueueEnqueueBuffer(audio_queue, buffer, 0, nullptr);
      },
      this, nullptr, nullptr, 0, &audio_queue_);
  if (!audio_queue_) {
    DLOG(ERROR) << "Failed to create AudioQueue output." << ret;
  }
  DCHECK(audio_queue_);

  AudioChannelLayout layout;
  memset(&layout, 0, sizeof(AudioChannelLayout));
  layout.mChannelLayoutTag = GetCoreAudioChannelLayoutTag(wanted_nb_channels);
  if (layout.mChannelLayoutTag != 0) {
    ret = AudioQueueSetProperty(audio_queue_, kAudioQueueProperty_ChannelLayout,
                                &layout, sizeof(layout));
    DCHECK_EQ(ret, noErr)
        << "AudioQueueSetProperty(kAudioQueueProperty_ChannelLayout)";
  }

  auto samples =
      std::max(kAudioMinBufferSize,
               2 << av_log2(wanted_sample_rate / kAudioMaxCallbacksPerSec));
  // bytes * channel * samples
  buffer_size_ = 2 * wanted_nb_channels * samples;
  DCHECK_GT(buffer_size_, 0);

  /* Make sure we can feed the device a minimum amount of time */
  double MINIMUM_AUDIO_BUFFER_TIME_MS = 15.0;
  const double msecs = (samples / ((double)wanted_sample_rate)) * 1000.0;

  int audio_buffer_num = 2;
  if (MINIMUM_AUDIO_BUFFER_TIME_MS < msecs) {
    audio_buffer_num = (int)std::ceil(MINIMUM_AUDIO_BUFFER_TIME_MS / msecs) * 2;
  }
  DLOG(INFO) << "Audio Buffer Size: " << buffer_size_
             << " audio_buffer_num: " << audio_buffer_num;
  InitializeBuffer(audio_buffer_num);

  device_output.fmt = AV_SAMPLE_FMT_S16;
  device_output.freq = wanted_sample_rate;
  device_output.channel_layout = wanted_channel_layout;
  device_output.channels = wanted_nb_channels;
  device_output.frame_size = av_samples_get_buffer_size(
      nullptr, device_output.channels, 1, device_output.fmt, 1);
  device_output.bytes_per_sec =
      av_samples_get_buffer_size(nullptr, device_output.channels,
                                 device_output.freq, device_output.fmt, 1);

  return buffer_size_;
}

void AudioRenderCoreAudio::InitializeBuffer(int audio_buffer_nb) {
  DCHECK(audio_queue_);
  DCHECK_GT(audio_buffer_nb, 0);

  audio_buffer_.resize(audio_buffer_nb);
  for (auto i = 0; i < audio_buffer_nb; i++) {
    auto result =
        AudioQueueAllocateBuffer(audio_queue_, buffer_size_, &audio_buffer_[i]);
    DCHECK_EQ(result, noErr) << "AudioQueueAllocateBuffer";
    memset(audio_buffer_[i]->mAudioData, 0,
           audio_buffer_[i]->mAudioDataBytesCapacity);
    audio_buffer_[i]->mAudioDataByteSize =
        audio_buffer_[i]->mAudioDataBytesCapacity;
    /* !!! FIXME: should we use AudioQueueEnqueueBufferWithParameters and
     * specify all frames be "trimmed" so these are immediately ready to refill
     * with SDL callback data? */
    result =
        AudioQueueEnqueueBuffer(audio_queue_, audio_buffer_[i], 0, nullptr);
    DCHECK_EQ(result, noErr) << "AudioQueueEnqueueBuffer";
  }
}

void AudioRenderCoreAudio::OnStart() const {
  if (audio_queue_) {
    AudioQueueStart(audio_queue_, nullptr);
  }
}

void AudioRenderCoreAudio::onStop() const {
  if (audio_queue_) {
    AudioQueuePause(audio_queue_);
  }
}

AudioRenderCoreAudio::~AudioRenderCoreAudio() {
  if (audio_queue_) {
    AudioQueueDispose(audio_queue_, true);
  }
  audio_buffer_.clear();
}

#endif
