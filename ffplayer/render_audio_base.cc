//
// Created by boyan on 21-2-17.
//

#include "render_audio_base.h"
#include <cmath>
#include <utility>

extern "C" {
#include "libswresample/swresample.h"
}

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB 20

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

AudioRenderBase::AudioRenderBase() = default;

void AudioRenderBase::Init(const std::shared_ptr<PacketQueue>& audio_queue,
                           std::shared_ptr<MediaClock> clock_ctx,
                           std::shared_ptr<MessageContext> message_context) {
  clock_ctx_ = std::move(clock_ctx);
  sample_queue = std::make_unique<FrameQueue>();
  sample_queue->Init(audio_queue.get(), SAMPLE_QUEUE_SIZE, 1);
  message_context_ = std::move(message_context);
}

AudioRenderBase::~AudioRenderBase() {
  swr_free(&swr_ctx);
  av_freep(&audio_buf1);
}

int AudioRenderBase::SynchronizeAudio(int nb_samples) {
  int wanted_nb_samples = nb_samples;

  if (clock_ctx_->GetMasterSyncType() != AV_SYNC_AUDIO_MASTER) {
    auto diff =
        clock_ctx_->GetAudioClock()->GetClock() - clock_ctx_->GetMasterClock();
    if (!std::isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
      audio_diff_cum = diff + audio_diff_avg_coef * audio_diff_cum;
      if (audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
        /* not enough measures to have a correct estimate */
        audio_diff_avg_count++;
      } else {
        /* estimate the A-V difference */
        auto avg_diff = audio_diff_cum * (1.0 - audio_diff_avg_coef);
        if (fabs(avg_diff) >= audio_diff_threshold) {
          wanted_nb_samples = nb_samples + (int)(diff * audio_src.freq);
          auto min_nb_samples =
              ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          auto max_nb_samples =
              ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
          wanted_nb_samples =
              av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
        }
        av_log(nullptr, AV_LOG_TRACE,
               "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n", diff,
               avg_diff, wanted_nb_samples - nb_samples, audio_clock_from_pts,
               audio_diff_threshold);
      }
    } else {
      /* too big difference : may be initial PTS errors, so reset A-V filter */
      audio_diff_avg_count = 0;
      audio_diff_cum = 0;
    }
  }
  return wanted_nb_samples;
}

int AudioRenderBase::PushFrame(AVFrame* frame, int pkt_serial) {
  auto* af = sample_queue->PeekWritable();
  if (!af) {
    return -1;
  }
  af->pts = (frame->pts == AV_NOPTS_VALUE)
                ? NAN
                : (double)frame->pts / frame->sample_rate;
  af->pos = frame->pkt_pos;
  af->serial = pkt_serial;
  af->duration = frame->nb_samples / (double)frame->sample_rate;
  av_frame_move_ref(af->frame, frame);
  sample_queue->Push();
  return 0;
}

int AudioRenderBase::AudioDecodeFrame() {
  if (paused_) {
    return -1;
  }
  Frame* af;
  int resampled_data_size;
  do {
    if (OnBeforeDecodeFrame() < 0) {
      return -1;
    }
    if (!(af = sample_queue->PeekReadable())) {
      return -1;
    }
    sample_queue->Next();
  } while (af->serial != *audio_queue_serial);

  auto data_size = av_samples_get_buffer_size(
      nullptr, (int)af->frame->channel_layout, af->frame->nb_samples,
      AVSampleFormat(af->frame->format), 1);
  auto dec_channel_layout =
      (af->frame->channel_layout &&
       af->frame->channels ==
           av_get_channel_layout_nb_channels(af->frame->channel_layout))
          ? af->frame->channel_layout
          : av_get_default_channel_layout(af->frame->channels);
  auto wanted_nb_samples = SynchronizeAudio(af->frame->nb_samples);

  if (af->frame->format != audio_src.fmt ||
      dec_channel_layout != audio_src.channel_layout ||
      af->frame->sample_rate != audio_src.freq ||
      (wanted_nb_samples != af->frame->nb_samples && !swr_ctx)) {
    swr_free(&swr_ctx);
    swr_ctx = swr_alloc_set_opts(
        nullptr, audio_tgt.channel_layout, audio_tgt.fmt, audio_tgt.freq,
        dec_channel_layout, static_cast<AVSampleFormat>(af->frame->format),
        af->frame->sample_rate, 0, nullptr);
    if (!swr_ctx || swr_init(swr_ctx) < 0) {
      av_log(nullptr, AV_LOG_ERROR,
             "Cannot create sample rate converter for conversion of %d Hz %s "
             "%d channels to %d Hz %s %d channels!\n",
             af->frame->sample_rate,
             av_get_sample_fmt_name(
                 static_cast<AVSampleFormat>(af->frame->format)),
             af->frame->channels, audio_tgt.freq,
             av_get_sample_fmt_name(audio_tgt.fmt), audio_tgt.channels);
      swr_free(&swr_ctx);
      return -1;
    }
    audio_src.channel_layout = dec_channel_layout;
    audio_src.channels = af->frame->channels;
    audio_src.freq = af->frame->sample_rate;
    audio_src.fmt = static_cast<AVSampleFormat>(af->frame->format);
  }

  if (swr_ctx) {
    const auto** in = (const uint8_t**)af->frame->extended_data;
    uint8_t** out = &audio_buf1;
    int64_t out_count =
        (int64_t)wanted_nb_samples * audio_tgt.freq / af->frame->sample_rate +
        256;
    int out_size = av_samples_get_buffer_size(nullptr, audio_tgt.channels,
                                              out_count, audio_tgt.fmt, 0);
    int len2;
    if (out_size < 0) {
      av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
      return -1;
    }
    if (wanted_nb_samples != af->frame->nb_samples) {
      if (swr_set_compensation(swr_ctx,
                               (wanted_nb_samples - af->frame->nb_samples) *
                                   audio_tgt.freq / af->frame->sample_rate,
                               wanted_nb_samples * audio_tgt.freq /
                                   af->frame->sample_rate) < 0) {
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
    resampled_data_size =
        len2 * audio_tgt.channels * av_get_bytes_per_sample(audio_tgt.fmt);
  } else {
    audio_buf = af->frame->data[0];
    resampled_data_size = data_size;
  }
#ifdef DEBUG
  auto audio_clock0 = audio_clock_from_pts;
#endif

  if (!isnan(af->pts)) {
    audio_clock_from_pts =
        af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
  } else {
    audio_clock_from_pts = NAN;
  }
  audio_clock_serial = af->serial;

#ifdef DEBUG
  {
    static double last_clock;
    printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
           is->audio_clock_ - last_clock, is->audio_clock_, audio_clock0);
    last_clock = is->audio_clock_;
  }
#endif

  return resampled_data_size;
}

void AudioRenderBase::Abort() {
  sample_queue->Signal();
}

int AudioRenderBase::OnBeforeDecodeFrame() {
  return 0;
}

void AudioRenderBase::Start() {
  paused_ = false;
  OnStart();
}

void AudioRenderBase::Stop() {
  paused_ = true;
  onStop();
}
