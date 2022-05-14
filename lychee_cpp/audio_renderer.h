//
// Created by Bin Yang on 2022/5/4.
//

#ifndef LYCHEE_PLAYER__AUDIO_RENDERER_H_
#define LYCHEE_PLAYER__AUDIO_RENDERER_H_

#include "base/basictypes.h"
#include "base/task_runner.h"

#include "audio_buffer.h"
#include "audio_device_info.h"
#include "audio_decode_config.h"

namespace lychee {

class AudioRendererHost {
 public:
  virtual void OnAudioRendererEnded() = 0;
  virtual void OnAudioRendererNeedMoreData() = 0;
};

class AudioRenderer {

 public:

  AudioRenderer(const media::TaskRunner &task_runner, AudioRendererHost *host);

  virtual ~AudioRenderer();

  using AudioDeviceInfoCallback = std::function<void(AudioDeviceInfo *)>;
  void Initialize(const AudioDecodeConfig &decode_config, AudioDeviceInfoCallback audio_device_info_callback);

  virtual void Play() = 0;

  virtual void Pause() = 0;

  [[nodiscard]] virtual bool IsPlaying() const = 0;

  [[nodiscard]] double CurrentTime() const;

  int Render(double delay, uint8 *stream, int len);

  void OnNewFrameAvailable(std::shared_ptr<AudioBuffer> buffer);

 protected:

  virtual bool OpenDevice(const AudioDecodeConfig &decode_config) = 0;

  AudioDeviceInfo audio_device_info_;

 private:
  media::TaskRunner task_runner_;

  std::deque<std::shared_ptr<AudioBuffer>> audio_buffer_;

  double audio_clock_time_;
  double audio_clock_update_time_stamp_;

  std::mutex mutex_;

  AudioRendererHost *host_;

  bool ended_;

  void AttemptReadFrame();

};

}

#endif //LYCHEE_PLAYER__AUDIO_RENDERER_H_
