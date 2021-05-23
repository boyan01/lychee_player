//
// Created by Bin Yang on 2021/5/23.
//
#include "macos_audio_renderer_sink.h"

#include "base/logging.h"

namespace media {

MacosAudioRendererSink::MacosAudioRendererSink() : task_runner_(TaskRunner::prepare_looper("macos_audio_renderer")) {

}

MacosAudioRendererSink::~MacosAudioRendererSink() = default;

void MacosAudioRendererSink::Initialize(int wanted_nb_channels,
                                        int wanted_sample_rate,
                                        AudioRendererSink::RenderCallback *render_callback) {
  DCHECK(render_callback);
  render_callback_ = render_callback;
  AudioStreamBasicDescription description{};
  description.mSampleRate = wanted_sample_rate;
  description.mFormatID = kAudioFormatLinearPCM;
  /**
   * https://www.jianshu.com/p/be78830eefbd
   *
   * AV_SAMPLE_FMT_S16   kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked
   * AV_SAMPLE_FMT_S16P  kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsNonInterleaved
   * AV_SAMPLE_FMT_FLT  kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
   * AV_SAMPLE_FMT_FLTP  kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved
   */
  description.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  description.mBytesPerPacket = 4;
  description.mFramesPerPacket = 1;
  description.mBytesPerFrame = 4;
  description.mChannelsPerFrame = 2;
  description.mBitsPerChannel = 16;
  auto ret = AudioQueueNewOutput(&description,
                                 [](void *in_user_data,
                                    AudioQueueRef audio_queue,
                                    AudioQueueBufferRef buffer) {
                                   auto *sink = static_cast<MacosAudioRendererSink *>(in_user_data);
                                   auto read = sink->ReadAudioData((uint8 *) buffer->mAudioData,
                                                                   int(buffer->mAudioDataBytesCapacity));
                                   buffer->mAudioDataByteSize = read;
                                   AudioQueueEnqueueBuffer(audio_queue, buffer, 0, nullptr);
                                 },
                                 this,
                                 nullptr,
                                 nullptr,
                                 0,
                                 &audio_queue_);

  if (!audio_queue_) {
    DLOG(ERROR) << "Failed to create AudioQueue output." << ret;
  }
  DCHECK(audio_queue_);
}

int MacosAudioRendererSink::ReadAudioData(uint8 *stream, int len) {
  return render_callback_->Render(0, stream, len);
}

bool MacosAudioRendererSink::SetVolume(double volume) {
  return false;
}

void MacosAudioRendererSink::Start() {
}

void MacosAudioRendererSink::Play() {
  DCHECK(audio_queue_);

  // TODO adjust buffer count.
  for (int i = 0; i < 10; ++i) {
    AudioQueueBufferRef buffer;
    // TODO adjust in_buffer_byte_size
    AudioQueueAllocateBuffer(audio_queue_, 2048, &buffer);
    DCHECK(buffer);
    memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
    buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(audio_queue_, buffer, 0, nullptr);
  }

  AudioQueueStart(audio_queue_, nullptr);

}

void MacosAudioRendererSink::Pause() {
  DCHECK(audio_queue_);
  AudioQueuePause(audio_queue_);
}

void MacosAudioRendererSink::Stop() {
  DCHECK(audio_queue_);
  AudioQueueDispose(audio_queue_, true);
}

}