//
// Created by Bin Yang on 2022/5/4.
//

#include "audio_renderer.h"

#include "base/logging.h"

extern "C" {
#include "libavutil/time.h" // NOLINT(modernize-deprecated-headers)
}

namespace lychee {

AudioRenderer::AudioRenderer(const media::TaskRunner &task_runner, AudioRendererHost *host)
    : task_runner_(task_runner), host_(host),
      audio_clock_update_time_stamp_(0), audio_clock_time_(0),
      audio_device_info_(), ended_(false) {

}

AudioRenderer::~AudioRenderer() = default;

int AudioRenderer::Render(double delay, uint8 *stream, int len) {
  DCHECK_GT(len, 0);
  DCHECK(stream);

  if (ended_) {
    task_runner_.PostTask(FROM_HERE, [this]() {
      host_->OnAudioRendererEnded();
    });
    ended_ = false;
    return -1;
  }

  double audio_clock_time = 0;
  auto render_callback_time = (double) av_gettime_relative() / 1000000.0;

  auto len_flush = 0;
  while (len_flush < len) {
    std::lock_guard<std::mutex> auto_lock(mutex_);
    if (audio_buffer_.empty()) {
      task_runner_.PostTask(FROM_HERE, [this]() {
        AttemptReadFrame();
      });
      break;
    }
    auto buffer = audio_buffer_.front();
    DCHECK(buffer);

    if (buffer->isEnd()) {
      audio_buffer_.pop_front();
      ended_ = true;
      break;
    }

    if (audio_clock_time == 0 && !std::isnan(buffer->pts())) {
      audio_clock_time = buffer->PtsFromCursor() - delay;
    }

    DCHECK(!buffer->IsConsumed()) << "buffer is consumed";
    auto flushed = buffer->Read(stream + len_flush, len - len_flush, 1);
    if (buffer->IsConsumed()) {
      audio_buffer_.pop_front();
      task_runner_.PostTask(FROM_HERE, [this]() {
        AttemptReadFrame();
      });
    }
    len_flush += flushed;
  }

  if (audio_clock_time != 0) {
    audio_clock_time_ = audio_clock_time;
    audio_clock_update_time_stamp_ = render_callback_time;
  } else {
    audio_clock_update_time_stamp_ = render_callback_time;
  }

  return len_flush;
}

void AudioRenderer::OnNewFrameAvailable(std::shared_ptr<AudioBuffer> buffer) {
  task_runner_.PostTask(FROM_HERE, [this, buffer(std::move(buffer))]() {
    std::lock_guard<std::mutex> auto_lock(mutex_);
    audio_buffer_.emplace_back(buffer);
    task_runner_.PostTask(FROM_HERE_WITH_EXPLICIT_FUNCTION("OnNewFrameAvailable#Task"), [this]() {
      AttemptReadFrame();
    });
  });
}

void AudioRenderer::AttemptReadFrame() {
  double audio_buffer_length = 0.0;
  {
    std::lock_guard<std::mutex> auto_lock(mutex_);
    for (const auto &item: audio_buffer_) {
      audio_buffer_length += item->DurationFromCursor();
    }
  }
  if (audio_buffer_length > 2.0) {
    return;
  }
  task_runner_.PostTask(FROM_HERE, [this]() {
    host_->OnAudioRendererNeedMoreData();
  });
}

void AudioRenderer::Initialize(const AudioDecodeConfig &decode_config,
                               AudioDeviceInfoCallback audio_device_info_callback) {
  task_runner_.PostTask(
      FROM_HERE,
      [this, audio_device_info_callback(std::move(audio_device_info_callback)),
          decode_config(decode_config)]() {
        auto ret = OpenDevice(decode_config);
        if (ret) {
          DLOG(INFO) << "OpenDevice success: ";
          DLOG(INFO) << "audio_device_info_ :"
                     << " bytes_per_sec = " << audio_device_info_.bytes_per_sec
                     << " freq = " << audio_device_info_.freq
                     << " channels = " << audio_device_info_.channels
                     << " channel_layout = " << audio_device_info_.channel_layout
                     << " fmt = " << av_get_sample_fmt_name(audio_device_info_.fmt);
          DLOG(INFO) << "decode_config :"
                     << " samples_per_second = " << decode_config.samples_per_second()
                     << " channels = " << decode_config.channels()
                     << " time_base = " << decode_config.time_base().den
                     << " channel_layout = " << decode_config.channel_layout()
                     << " CodecId = " << avcodec_get_name(decode_config.CodecId());
        }
        DLOG(INFO) << "OpenDevice ret: " << ret << ", audio_device_info_: " << audio_device_info_.freq;
        audio_device_info_callback(ret ? &audio_device_info_ : nullptr);
      });
}

double AudioRenderer::CurrentTime() const {
  if (!IsPlaying()) {
    return audio_clock_time_;
  }
  if (audio_clock_update_time_stamp_ == 0) {
    return audio_clock_time_;
  }
  auto now = (double) av_gettime_relative() / 1000000.0;
  return audio_clock_time_ + (now - audio_clock_update_time_stamp_);
}

}