//
// Created by boyan on 2021/2/12.
//

#include "ffp_audio_render.h"
#include "ffp_data_source.h"
#include <string>

#include <cmath>
#include <utility>

extern "C" {
#include <libswresample/swresample.h>
}

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB 20

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB 20


/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

int AudioRender::Open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate) {
  SDL_AudioSpec wanted_spec, spec;
  const char *env;
  static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
  static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
  int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

  env = SDL_getenv("SDL_AUDIO_CHANNELS");
  if (env) {
    wanted_nb_channels = std::atoi(env); // NOLINT(cert-err34-c)
    wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
  }
  if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
    wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
  }
  wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
  wanted_spec.channels = wanted_nb_channels;
  wanted_spec.freq = wanted_sample_rate;
  if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
    av_log(nullptr, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
    return -1;
  }
  while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
    next_sample_rate_idx--;
  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.silence = 0;
  wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
                              2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
  wanted_spec.callback = [](void *userdata, Uint8 *stream, int len) {
    auto *render = static_cast<AudioRender *>(userdata);
    render->AudioCallback(stream, len);
  };
  wanted_spec.userdata = this;
  while (!(audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
      SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
    av_log(nullptr, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
           wanted_spec.channels, wanted_spec.freq, SDL_GetError());
    wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
    if (!wanted_spec.channels) {
      wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
      wanted_spec.channels = wanted_nb_channels;
      if (!wanted_spec.freq) {
        av_log(nullptr, AV_LOG_ERROR,
               "No more combinations to try, audio open failed\n");
        return -1;
      }
    }
    wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
  }

  if (spec.format != AUDIO_S16SYS) {
    av_log(nullptr, AV_LOG_ERROR,
           "SDL advised audio format %d is not supported!\n", spec.format);
    return -1;
  }
  if (spec.channels != wanted_spec.channels) {
    wanted_channel_layout = av_get_default_channel_layout(spec.channels);
    if (!wanted_channel_layout) {
      av_log(nullptr, AV_LOG_ERROR,
             "SDL advised channel count %d is not supported!\n", spec.channels);
      return -1;
    }
  }

  audio_tgt.fmt = AV_SAMPLE_FMT_S16;
  audio_tgt.freq = spec.freq;
  audio_tgt.channel_layout = wanted_channel_layout;
  audio_tgt.channels = spec.channels;
  audio_tgt.frame_size = av_samples_get_buffer_size(nullptr, audio_tgt.channels, 1,
                                                    audio_tgt.fmt,
                                                    1);
  audio_tgt.bytes_per_sec = av_samples_get_buffer_size(nullptr, audio_tgt.channels,
                                                       audio_tgt.freq,
                                                       audio_tgt.fmt, 1);
  if (audio_tgt.bytes_per_sec <= 0 || audio_tgt.frame_size <= 0) {
    av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
    return -1;
  }

  audio_src = audio_tgt;

  audio_hw_buf_size = spec.size;
  audio_buf_size = 0;
  audio_buf_index = 0;

  /* init averaging filter */
  audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
  audio_diff_avg_count = 0;

  /* since we do not have a precise anough audio FIFO fullness,
we correct audio sync only if larger than this threshold */
  audio_diff_threshold = audio_hw_buf_size / (double) audio_tgt.bytes_per_sec;

  return spec.size;
}

int AudioRender::PushFrame(AVFrame *frame, int pkt_serial) {
  auto *af = sample_queue->PeekWritable();
  if (!af) {
    return -1;
  }
  af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts / (double) frame->sample_rate;
  af->pos = frame->pkt_pos;
  af->serial = pkt_serial;
  af->duration = frame->nb_samples / (double) frame->sample_rate;
  av_frame_move_ref(af->frame, frame);
  sample_queue->Push();
  return 0;
}

void AudioRender::AudioCallback(Uint8 *stream, int len) {
  audio_callback_time = av_gettime_relative();
  while (len > 0) {
    if (audio_buf_index >= audio_buf_size) {
      auto audio_size = AudioDecodeFrame();
      if (audio_size < 0) {
        /* if error, just output silence */
        audio_buf = nullptr;
        audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / audio_tgt.frame_size * audio_tgt.frame_size;
      } else {
        audio_buf_size = audio_size;
      }
      audio_buf_index = 0;
    }
    auto len_flush = (audio_buf_size - audio_buf_index);
    if (len_flush > len) {
      len_flush = len;
    }

    if (!mute && audio_buf && audio_volume == SDL_MIX_MAXVOLUME) {
      memcpy(stream, (uint8_t *) audio_buf + audio_buf_index, len_flush);
    } else {
      memset(stream, 0, len_flush);
      if (!mute && audio_buf) {
        SDL_MixAudioFormat(stream, (uint8_t *) audio_buf + audio_buf_index, AUDIO_S16SYS, len_flush,
                           audio_volume);
      }
    }

    len -= len_flush;
    stream += len_flush;
    audio_buf_index += len_flush;
  }

  audio_write_buf_size = audio_buf_size - audio_buf_index;
  if (!isnan(audio_clock_from_pts)) {
    clock_ctx_->GetAudioClock()->SetClockAt(
        audio_clock_from_pts - (double) (2 * audio_hw_buf_size + audio_write_buf_size) /
            audio_tgt.bytes_per_sec, audio_clock_serial,
        audio_callback_time / 1000000.0);
    clock_ctx_->GetExtClock()->Sync(clock_ctx_->GetAudioClock());
  }
}

int AudioRender::AudioDecodeFrame() {
  if (paused) {
    return -1;
  }
  Frame *af;
  int resampled_data_size;
  do {
#if defined(_WIN32)
    while (sample_queue->NbRemaining() == 0) {
      if ((av_gettime_relative() - audio_callback_time) >
          1000000LL * audio_hw_buf_size / audio_tgt.bytes_per_sec / 2)
        return -1;
      av_usleep(1000);
    }
#endif
    if (!(af = sample_queue->PeekReadable())) {
      return -1;
    }
    sample_queue->Next();
  } while (af->serial != *audio_queue_serial);

  auto data_size = av_samples_get_buffer_size(nullptr, af->frame->channel_layout, af->frame->nb_samples,
                                              AVSampleFormat(af->frame->format), 1);
  auto dec_channel_layout =
      (af->frame->channel_layout &&
          af->frame->channels == av_get_channel_layout_nb_channels(af->frame->channel_layout))
      ? af->frame->channel_layout : av_get_default_channel_layout(af->frame->channels);
  auto wanted_nb_samples = SynchronizeAudio(af->frame->nb_samples);

  if (af->frame->format != audio_src.fmt ||
      dec_channel_layout != audio_src.channel_layout ||
      af->frame->sample_rate != audio_src.freq ||
      (wanted_nb_samples != af->frame->nb_samples && !swr_ctx)) {
    swr_free(&swr_ctx);
    swr_ctx = swr_alloc_set_opts(nullptr, audio_tgt.channel_layout, audio_tgt.fmt, audio_tgt.freq,
                                 dec_channel_layout, static_cast<AVSampleFormat>(af->frame->format),
                                 af->frame->sample_rate,
                                 0, nullptr);
    if (!swr_ctx || swr_init(swr_ctx) < 0) {
      av_log(nullptr,
             AV_LOG_ERROR,
             "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
             af->frame->sample_rate,
             av_get_sample_fmt_name(static_cast<AVSampleFormat>(af->frame->format)),
             af->frame->channels,
             audio_tgt.freq,
             av_get_sample_fmt_name(audio_tgt.fmt),
             audio_tgt.channels);
      swr_free(&swr_ctx);
      return -1;
    }
    audio_src.channel_layout = dec_channel_layout;
    audio_src.channels = af->frame->channels;
    audio_src.freq = af->frame->sample_rate;
    audio_src.fmt = static_cast<AVSampleFormat>(af->frame->format);
  }

  if (swr_ctx) {
    const auto **in = (const uint8_t **) af->frame->extended_data;
    uint8_t **out = &audio_buf1;
    int64_t out_count = (int64_t) wanted_nb_samples * audio_tgt.freq / af->frame->sample_rate + 256;
    int out_size = av_samples_get_buffer_size(nullptr, audio_tgt.channels, out_count, audio_tgt.fmt, 0);
    int len2;
    if (out_size < 0) {
      av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
      return -1;
    }
    if (wanted_nb_samples != af->frame->nb_samples) {
      if (swr_set_compensation(swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * audio_tgt.freq /
                                   af->frame->sample_rate,
                               wanted_nb_samples * audio_tgt.freq / af->frame->sample_rate) < 0) {
        av_log(nullptr, AV_LOG_ERROR, "swr_set_compensation() failed\n");
        return -1;
      }
    }
    av_fast_malloc(&audio_buf1, &audio_buf1_size, out_size);
    if (!audio_buf1)
      return AVERROR(ENOMEM);
    len2 = swr_convert(swr_ctx, out, out_count, in, af->frame->nb_samples);
    if (len2 < 0) {
      av_log(nullptr, AV_LOG_ERROR, "swr_convert() failed\n");
      return -1;
    }
    if (len2 == out_count) {
      av_log(nullptr, AV_LOG_WARNING, "audio buffer is probably too small\n");
      if (swr_init(swr_ctx) < 0)
        swr_free(&swr_ctx);
    }
    audio_buf = audio_buf1;
    resampled_data_size = len2 * audio_tgt.channels * av_get_bytes_per_sample(audio_tgt.fmt);
  } else {
    audio_buf = af->frame->data[0];
    resampled_data_size = data_size;
  }
#ifdef DEBUG
  auto audio_clock0 = audio_clock_from_pts;
#endif

  if (!isnan(af->pts)) {
    audio_clock_from_pts = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
  } else {
    audio_clock_from_pts = NAN;
  }
  audio_clock_serial = af->serial;

#ifdef DEBUG
  {
      static double last_clock;
      printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
             is->audio_clock_ - last_clock,
             is->audio_clock_, audio_clock0);
      last_clock = is->audio_clock_;
  }
#endif

  return resampled_data_size;
}

int AudioRender::SynchronizeAudio(int nb_samples) {
  int wanted_nb_samples = nb_samples;

  if (clock_ctx_->GetMasterSyncType() != AV_SYNC_AUDIO_MASTER) {
    auto diff = clock_ctx_->GetAudioClock()->GetClock() - clock_ctx_->GetMasterClock();
    if (!std::isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
      audio_diff_cum = diff + audio_diff_avg_coef * audio_diff_cum;
      if (audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
        /* not enough measures to have a correct estimate */
        audio_diff_avg_count++;
      } else {
        /* estimate the A-V difference */
        auto avg_diff = audio_diff_cum * (1.0 - audio_diff_avg_coef);
        if (fabs(avg_diff) >= audio_diff_threshold) {
          wanted_nb_samples = nb_samples + (int) (diff * audio_src.freq);
          auto min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          auto max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
        }
        av_log(nullptr, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
               diff, avg_diff, wanted_nb_samples - nb_samples,
               audio_clock_from_pts, audio_diff_threshold);
      }
    } else {
      /* too big difference : may be initial PTS errors, so reset A-V filter */
      audio_diff_avg_count = 0;
      audio_diff_cum = 0;
    }
  }
  return wanted_nb_samples;
}

void AudioRender::Start() const {
  SDL_PauseAudioDevice(audio_dev, 0);
}

void AudioRender::Pause() const {
  SDL_PauseAudioDevice(audio_dev, 1);
}

AudioRender::AudioRender(const std::shared_ptr<PacketQueue> &audio_queue, std::shared_ptr<ClockContext> clock_ctx)
    : clock_ctx_(std::move(clock_ctx)) {
  sample_queue = std::make_unique<FrameQueue>();
  sample_queue->Init(audio_queue.get(), SAMPLE_QUEUE_SIZE, 1);
}

AudioRender::~AudioRender() {
  SDL_CloseAudioDevice(audio_dev);
  swr_free(&swr_ctx);
  av_freep(&audio_buf1);
}

bool AudioRender::IsMute() const { return mute; }

void AudioRender::SetMute(bool _mute) {
  mute = _mute;
}

void AudioRender::SetVolume(int _volume) {
  _volume = av_clip(_volume, 0, 100);
  _volume = av_clip(SDL_MIX_MAXVOLUME * _volume / 100, 0, SDL_MIX_MAXVOLUME);
  audio_volume = _volume;
}

int AudioRender::GetVolume() const {
  int volume = audio_volume * 100 / SDL_MIX_MAXVOLUME;
  return av_clip(volume, 0, 100);
}

void AudioRender::Abort() {
  sample_queue->Signal();
}

