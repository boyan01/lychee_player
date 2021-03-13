//
// Created by yangbin on 2021/2/13.
//

#include "media_player.h"

#ifndef _FLUTTER_MEDIA_ANDROID
#define MEDIA_SDL_ENABLE
#endif

#ifdef MEDIA_SDL_ENABLE
#include <SDL2/SDL.h>

#include <utility>
#endif

extern "C" {
#include "libavutil/bprint.h"
}

MediaPlayer::MediaPlayer(
    std::unique_ptr<VideoRenderBase> video_render,
    std::unique_ptr<BasicAudioRender> audio_render
) : video_render_(std::move(video_render)), audio_render_(std::move(audio_render)), player_mutex_() {
  message_context = std::make_shared<MessageContext>();
  message_context->message_callback = [this](int what, int64_t arg1, int64_t arg2) {
    switch (what) { // NOLINT(hicpp-multiway-paths-covered)
      case MEDIA_MSG_DO_SOME_WORK: {
        DoSomeWork();
        break;
      }
      default: {
        if (message_callback_external_) {
          message_callback_external_(what, arg1, arg2);
        }
        break;
      }
    }
  };

  audio_pkt_queue = std::make_shared<PacketQueue>();
  video_pkt_queue = std::make_shared<PacketQueue>();
  subtitle_pkt_queue = std::make_shared<PacketQueue>();

  auto sync_type_confirm = [this](int av_sync_type) -> int {
    if (data_source == nullptr) {
      return av_sync_type;
    }
    if (av_sync_type == AV_SYNC_VIDEO_MASTER) {
      if (data_source->ContainVideoStream()) {
        return AV_SYNC_VIDEO_MASTER;
      } else {
        return AV_SYNC_AUDIO_MASTER;
      }
    } else if (av_sync_type == AV_SYNC_AUDIO_MASTER) {
      if (data_source->ContainAudioStream()) {
        return AV_SYNC_AUDIO_MASTER;
      } else {
        return AV_SYNC_EXTERNAL_CLOCK;
      }
    } else {
      return AV_SYNC_EXTERNAL_CLOCK;
    }
  };
  clock_context = std::make_shared<MediaClock>(&audio_pkt_queue->serial, &video_pkt_queue->serial,
                                               sync_type_confirm);

  if (audio_render_) {
    audio_render_->SetRenderCallback([this]() { message_context->NotifyMsg(MEDIA_MSG_DO_SOME_WORK); });
    audio_render_->Init(audio_pkt_queue, clock_context);
  }
  if (video_render_) {
    video_render_->SetRenderCallback([this]() { message_context->NotifyMsg(MEDIA_MSG_DO_SOME_WORK); });
    video_render_->Init(video_pkt_queue, clock_context, message_context);
  }

  decoder_context = std::make_shared<DecoderContext>(audio_render_, video_render_, clock_context, [this]() {
    OnDecoderBlocking();
  });
}

MediaPlayer::~MediaPlayer() = default;

void MediaPlayer::SetPlayWhenReady(bool play_when_ready) {
  if (play_when_ready_ == play_when_ready) {
    return;
  }
  play_when_ready_ = play_when_ready;
  if (data_source) {
    data_source->paused = !play_when_ready;
  }
  if (!play_when_ready) {
    StopRenders();
  } else {
    StartRenders();
  }
}

void MediaPlayer::PauseClock(bool pause) {
  if (clock_context->paused == pause) {
    return;
  }
  if (clock_context->paused) {
    if (video_render_) {
      video_render_->frame_timer +=
          av_gettime_relative() / 1000000.0 - clock_context->GetVideoClock()->last_updated;
    }
    if (data_source->read_pause_return != AVERROR(ENOSYS)) {
      clock_context->GetVideoClock()->paused = 0;
    }
    clock_context->GetVideoClock()->SetClock(clock_context->GetVideoClock()->GetClock(),
                                             clock_context->GetVideoClock()->serial);
  }
  clock_context->GetExtClock()->SetClock(clock_context->GetExtClock()->GetClock(),
                                         clock_context->GetExtClock()->serial);
  clock_context->paused = pause;
  clock_context->GetExtClock()->paused = pause;
  clock_context->GetAudioClock()->paused = pause;
  clock_context->GetVideoClock()->paused = pause;
}

int MediaPlayer::OpenDataSource(const char *filename) {
  if (data_source) {
    av_log(nullptr, AV_LOG_ERROR, "can not open file multi-times.\n");
    return -1;
  }

  data_source = std::make_unique<DataSource>(filename, nullptr);
  data_source->configuration = start_configuration;
  data_source->audio_queue = audio_pkt_queue;
  data_source->video_queue = video_pkt_queue;
  data_source->subtitle_queue = subtitle_pkt_queue;
  data_source->ext_clock = clock_context->GetAudioClock();
  data_source->decoder_ctx = decoder_context;
  data_source->msg_ctx = message_context;

  data_source->on_new_packet_send_ = [this]() {
    message_context->NotifyMsg(MEDIA_MSG_DO_SOME_WORK);
  };

  data_source->Open();
  ChangePlaybackState(MediaPlayerState::BUFFERING);
  return 0;
}

void MediaPlayer::DumpStatus() {
  AVBPrint buf;
  static int64_t last_time;
  int64_t cur_time;
  int aqsize, vqsize, sqsize;
  double av_diff;

  cur_time = av_gettime_relative();
  if (!last_time || (cur_time - last_time) >= 30000) {
    aqsize = 0;
    vqsize = 0;
    sqsize = 0;
    if (data_source->ContainAudioStream())
      aqsize = audio_pkt_queue->size;
    if (data_source->ContainVideoStream())
      vqsize = video_pkt_queue->size;
    if (data_source->ContainSubtitleStream())
      sqsize = subtitle_pkt_queue->size;
    av_diff = 0;
    if (data_source->ContainAudioStream() && data_source->ContainVideoStream())
      av_diff = clock_context->GetAudioClock()->GetClock() -
          clock_context->GetVideoClock()->GetClock();
    else if (data_source->ContainVideoStream())
      av_diff = clock_context->GetMasterClock() - clock_context->GetVideoClock()->GetClock();
    else if (data_source->ContainAudioStream())
      av_diff = clock_context->GetMasterClock() - clock_context->GetAudioClock()->GetClock();

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprintf(&buf,
               "%7.2f/%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"
               PRId64
               "/%"
               PRId64
               "   \r",
               GetCurrentPosition(), GetDuration(),
               (data_source->ContainAudioStream()
                   && data_source->ContainAudioStream()) ? "A-V"
                                                         : (data_source->ContainVideoStream()
                                                            ? "M-V"
                                                            : (data_source->ContainAudioStream()
                                                               ? "M-A"
                                                               : "   ")),
               av_diff,
        /*video_render->frame_drop_count + video_render->frame_drop_count_pre*/ 0,
               aqsize / 1024,
               vqsize / 1024,
               sqsize,
        /*data_source->ContainVideoStream() ? decoder_context->.avctx->pts_correction_num_faulty_dts :*/
               0ll,
        /* is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts :*/ 0ll);

    if (start_configuration.show_status == 1 && AV_LOG_INFO > av_log_get_level())
      fprintf(stderr, "%s", buf.str);
    else
      av_log(nullptr, AV_LOG_INFO, "%s", buf.str);

    fflush(stderr);
    av_bprint_finalize(&buf, nullptr);

    last_time = cur_time;
  }

}

double MediaPlayer::GetCurrentPosition() {
  double position = clock_context->GetMasterClock();
  if (isnan(position)) {
    if (data_source) {
      position = (double) data_source->GetSeekPosition() / AV_TIME_BASE;
    } else {
      position = 0;
    }
  }
  return position;
}

int MediaPlayer::GetVolume() {
  CHECK_VALUE_WITH_RETURN(audio_render_, 0);
  return audio_render_->GetVolume();
}

void MediaPlayer::SetVolume(int volume) {
  CHECK_VALUE(audio_render_);
  audio_render_->SetVolume(volume);
}

void MediaPlayer::SetMute(bool mute) {
  CHECK_VALUE(audio_render_);
  audio_render_->SetMute(mute);
}

bool MediaPlayer::IsMuted() {
  CHECK_VALUE_WITH_RETURN(audio_render_, true);
  return audio_render_->IsMute();
}

double MediaPlayer::GetDuration() {
  CHECK_VALUE_WITH_RETURN(data_source, -1);
  return data_source->GetDuration();
}

void MediaPlayer::Seek(double position) {
  CHECK_VALUE(data_source);
  ChangePlaybackState(MediaPlayerState::BUFFERING);
  data_source->Seek(position);
}

void MediaPlayer::SeekToChapter(int chapter) {
  CHECK_VALUE(data_source);
  data_source->SeekToChapter(chapter);
}

int MediaPlayer::GetCurrentChapter() {
  CHECK_VALUE_WITH_RETURN(data_source, -1);
  int64_t pos = GetCurrentPosition() * AV_TIME_BASE;
  return data_source->GetChapterByPosition(pos);
}

int MediaPlayer::GetChapterCount() {
  CHECK_VALUE_WITH_RETURN(data_source, -1);
  return data_source->GetChapterCount();
}

void MediaPlayer::SetMessageHandleCallback(std::function<void(int what, int64_t arg1, int64_t arg2)> message_callback) {
  message_callback_external_ = std::move(message_callback);
}

double MediaPlayer::GetVideoAspectRatio() {
  CHECK_VALUE_WITH_RETURN(video_render_, 0);
  return video_render_->GetVideoAspectRatio();
}

const char *MediaPlayer::GetUrl() {
  CHECK_VALUE_WITH_RETURN(data_source, nullptr);
  return data_source->GetFileName();
}

const char *MediaPlayer::GetMetadataDict(const char *key) {
  CHECK_VALUE_WITH_RETURN(data_source, nullptr);
  return data_source->GetMetadataDict(key);
}

void MediaPlayer::GlobalInit() {
  av_log_set_flags(AV_LOG_SKIP_REPEATED);
  av_log_set_level(AV_LOG_INFO);
  /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
  avdevice_register_all();
#endif
  avformat_network_init();

#ifdef MEDIA_SDL_ENABLE
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
    av_log(nullptr, AV_LOG_ERROR, "SDL fails to initialize audio subsystem!\n%s", SDL_GetError());
  else
    av_log(nullptr, AV_LOG_DEBUG, "SDL Audio was initialized fine!\n");
#endif

  flush_pkt = new AVPacket;
  av_init_packet(flush_pkt);
  flush_pkt->data = (uint8_t *) &flush_pkt;
}

VideoRenderBase *MediaPlayer::GetVideoRender() {
  CHECK_VALUE_WITH_RETURN(video_render_, nullptr);
  return video_render_.get();
}

void MediaPlayer::OnDecoderBlocking() {

}

void MediaPlayer::DoSomeWork() {
  std::lock_guard<std::mutex> lock(player_mutex_);
  bool render_allow_playback = true;
  if (audio_render_) {
    render_allow_playback &= audio_render_->IsReady();
  }
  if (video_render_) {
    render_allow_playback &= video_render_->IsReady();
  }

  if (player_state_ == MediaPlayerState::READY && !render_allow_playback) {
    ChangePlaybackState(MediaPlayerState::BUFFERING);
    StopRenders();
  } else if (player_state_ == MediaPlayerState::BUFFERING && ShouldTransitionToReadyState(render_allow_playback)) {
    ChangePlaybackState(MediaPlayerState::READY);
    if (play_when_ready_) {
      StartRenders();
    }
  }
}

void MediaPlayer::ChangePlaybackState(MediaPlayerState state) {
  if (player_state_ == state) {
    return;
  }
  player_state_ = state;
  message_context->NotifyMsg(FFP_MSG_PLAYBACK_STATE_CHANGED, int(player_state_));
}

void MediaPlayer::StopRenders() {
  av_log(nullptr, AV_LOG_INFO, "StopRenders\n");
  PauseClock(true);
  if (audio_render_) {
    audio_render_->Stop();
  }
  if (video_render_) {
    video_render_->Stop();
  }
}

void MediaPlayer::StartRenders() {
  av_log(nullptr, AV_LOG_INFO, "StartRenders\n");
  PauseClock(false);
  if (audio_render_) {
    audio_render_->Start();
  }
  if (video_render_) {
    video_render_->Start();
  }
}

bool MediaPlayer::ShouldTransitionToReadyState(bool render_allow_play) {
  if (!render_allow_play) {
    return false;
  }
  if (data_source->IsReadComplete()) {
    return true;
  }

  bool ready = true;
  if (audio_render_ && data_source->ContainAudioStream()) {
    ready &= audio_pkt_queue->nb_packets > 2;
  }
  if (video_render_ && data_source->ContainVideoStream()) {
    ready &= video_pkt_queue->nb_packets > 2;
  }
  return ready;
}


