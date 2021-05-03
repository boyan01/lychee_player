//
// Created by yangbin on 2021/2/13.
//

#include "media_player.h"
#include "decoder_ctx.h"

#include "base/logging.h"
#include "base/lambda.h"

extern "C" {
#include "libavutil/bprint.h"
}

namespace media {

MediaPlayer::MediaPlayer(
    std::unique_ptr<VideoRenderBase> video_render,
    std::shared_ptr<AudioRendererSink> audio_renderer_sink
) : video_render_(std::move(video_render)) {
  task_runner_ = TaskRunner::prepare_looper("media_player");
  task_runner_->PostTask(FROM_HERE, [&]() {
    Initialize();
  });
  decoder_task_runner_ = TaskRunner::prepare_looper("decoder");
  audio_renderer_ = std::make_shared<AudioRenderer>(decoder_task_runner_, std::move(audio_renderer_sink));
}

void MediaPlayer::Initialize() {
  DCHECK_EQ(state_, kUninitialized);
  DCHECK(task_runner_->BelongsToCurrentThread());

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

  decoder_context = std::make_shared<DecoderContext>(clock_context, [this]() {
    ChangePlaybackState(MediaPlayerState::BUFFERING);
//    StopRenders();
  });

  state_ = kIdle;

}

MediaPlayer::~MediaPlayer() {
  task_runner_->Quit();
};

void MediaPlayer::SetPlayWhenReady(bool play_when_ready) {
  std::lock_guard<std::mutex> lock_guard(player_mutex_);
  if (play_when_ready_pending_ == play_when_ready) {
    return;
  }
  play_when_ready_pending_ = play_when_ready;
  task_runner_->PostTask(FROM_HERE, [&]() {
    SetPlayWhenReadyTask(play_when_ready_pending_);
  });
}

void MediaPlayer::SetPlayWhenReadyTask(bool play_when_ready) {
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
      video_render_->frame_timer_ += get_relative_time() - clock_context->GetVideoClock()->last_updated;
    }
    if (data_source && data_source->read_pause_return != AVERROR(ENOSYS)) {
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
  task_runner_->PostTask(FROM_HERE, [&, filename]() {
    OpenDataSourceTask(filename);
  });
  return 0;
}

void MediaPlayer::OpenDataSourceTask(const char *filename) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(!data_source) << "can not open file multi-times.";
  DCHECK_EQ(state_, kIdle);

  DLOG(INFO) << "open file: " << filename;

  data_source = std::make_unique<DataSource1>(filename, nullptr);
  data_source->configuration = start_configuration;
  data_source->audio_queue = audio_pkt_queue;
  data_source->video_queue = video_pkt_queue;
  data_source->subtitle_queue = subtitle_pkt_queue;
  data_source->ext_clock = clock_context->GetAudioClock();
  data_source->decoder_ctx = decoder_context;

  data_source->on_new_packet_send_ = [this]() {
    CheckBuffering();
  };

  data_source->Open(bind_weak(&MediaPlayer::OnDataSourceOpen, shared_from_this()));
  ChangePlaybackState(MediaPlayerState::BUFFERING);
  state_ = kPreparing;
}

void MediaPlayer::OnDataSourceOpen(int open_status) {
  DCHECK_EQ(state_, kPreparing);
  if (open_status >= 0) {
    state_ = kPrepared;
    DLOG(INFO) << "Open DataSource Succeed";
    task_runner_->PostTask(FROM_HERE, bind_weak(&MediaPlayer::InitVideoRender, shared_from_this()));
  } else {
    state_ = kIdle;
  }
}

void MediaPlayer::InitVideoRender() {
  if (data_source->ContainVideoStream()) {
    auto video_decoder = std::make_shared<VideoDecoder>(decoder_task_runner_);
    auto ret = video_decoder->Initialize(data_source->video_decode_config(),
                                         data_source->video_demuxer_stream(),
                                         bind_weak(&VideoRenderBase::Flush, video_render_));
    if (ret >= 0) {
      video_render_->Initialize(this, data_source->video_demuxer_stream(),
                                clock_context, std::move(video_decoder));
    } else {
      DLOG(ERROR) << "Open Video Decoder Failed: " << ret;
    }
  }
  task_runner_->PostTask(FROM_HERE, bind_weak(&MediaPlayer::InitAudioRender, shared_from_this()));
}

void MediaPlayer::InitAudioRender() {
  if (data_source->ContainAudioStream()) {
    auto audio_decoder_stream = std::make_shared<AudioDecoderStream>(
        std::make_unique<DecoderStreamTraits<DemuxerStream::Audio>>(),
        decoder_task_runner_
    );
    audio_renderer_->Initialize(data_source->audio_demuxer_stream(), clock_context,
                                bind_weak(&MediaPlayer::OnAudioRendererInitialized, shared_from_this()));
  }
}

void MediaPlayer::OnAudioRendererInitialized(bool success) {
  DLOG(INFO) << success;
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
  if (state_ == kUninitialized) {
    return 0;
  }

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
  CHECK_VALUE_WITH_RETURN(audio_renderer_, 0);
//  return audio_render_->GetVolume();
}

void MediaPlayer::SetVolume(int volume) {
  CHECK_VALUE(audio_renderer_);
//  audio_render_->SetVolume(volume);
}

void MediaPlayer::SetMute(bool mute) {
  CHECK_VALUE(audio_renderer_);
//  audio_render_->SetMute(mute);
}

bool MediaPlayer::IsMuted() {
  CHECK_VALUE_WITH_RETURN(audio_renderer_, true);
//  return audio_render_->IsMute();
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

}

VideoRenderBase *MediaPlayer::GetVideoRender() {
  CHECK_VALUE_WITH_RETURN(video_render_, nullptr);
  return video_render_.get();
}

void MediaPlayer::DoSomeWork() {
  std::lock_guard<std::mutex> lock(player_mutex_);
  bool render_allow_playback = true;
  if (audio_renderer_) {
//    render_allow_playback &= audio_render_->IsReady();
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
}

void MediaPlayer::StopRenders() {
  av_log(nullptr, AV_LOG_INFO, "StopRenders\n");
  PauseClock(true);
  if (audio_renderer_) {
//    audio_render_->Stop();
  }
  if (video_render_) {
    video_render_->Stop();
  }
}

void MediaPlayer::StartRenders() {
  av_log(nullptr, AV_LOG_INFO, "StartRenders\n");
  PauseClock(false);
  if (audio_renderer_) {
//    audio_render_->Start();
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
  if (audio_renderer_ && data_source->ContainAudioStream()) {
    ready &= audio_pkt_queue->nb_packets > 2;
  }
  if (video_render_ && data_source->ContainVideoStream()) {
    ready &= video_pkt_queue->nb_packets > 2;
  }
  return ready;
}

static inline bool check_queue_is_ready(const std::shared_ptr<PacketQueue> &queue, bool has_stream) {
  static const int min_frames = 2;
  return queue->nb_packets > min_frames || !has_stream || queue->abort_request;
}

void MediaPlayer::CheckBuffering() {
  if (data_source->IsReadComplete()) {
    double duration = GetDuration();
    if (duration <= 0 && audio_pkt_queue->last_pkt) {
      auto d = audio_pkt_queue->last_pkt->pkt.pts + audio_pkt_queue->last_pkt->pkt.duration;
      duration = av_q2d(audio_pkt_queue->time_base) * d;
    }
    if (duration <= 0 && video_pkt_queue->last_pkt) {
      duration = av_q2d(video_pkt_queue->time_base) *
          (int64_t) (video_pkt_queue->last_pkt->pkt.pts + video_pkt_queue->last_pkt->pkt.duration);
    }
    buffered_position_ = duration;
    ChangePlaybackState(MediaPlayerState::READY);
    return;
  }

  static const double BUFFERING_CHECK_DELAY = (0.500);
  static const double BUFFERING_CHECK_NO_RENDERING_DELAY = (0.020);

  auto current_time = get_relative_time();
  auto step = player_state_ == MediaPlayerState::BUFFERING ? BUFFERING_CHECK_NO_RENDERING_DELAY : BUFFERING_CHECK_DELAY;
  if (current_time - buffering_check_last_stamp_ < step) {
    // It is not time to check buffering state.
    return;
  }
  if (player_state_ == MediaPlayerState::END || player_state_ == MediaPlayerState::IDLE) {
    return;
  }
  buffering_check_last_stamp_ = current_time;

  auto check_packet = [](const std::shared_ptr<PacketQueue> &queue, int &nb_packets, double &cached_position) {
    nb_packets = FFMIN(nb_packets, queue->nb_packets);
    auto last_position = queue->last_pkt->pkt.pts * av_q2d(queue->time_base);
    cached_position = FFMIN(cached_position, last_position);
  };

  double cached_position = INT_MAX;
  int nb_packets = INT_MAX;

  if (data_source->ContainAudioStream() && audio_pkt_queue->last_pkt) {
    check_packet(audio_pkt_queue, nb_packets, cached_position);
  }
  if (data_source->ContainVideoStream() && video_pkt_queue->last_pkt) {
    check_packet(video_pkt_queue, nb_packets, cached_position);
  }

  if (cached_position == INT_MAX) {
    av_log(nullptr, AV_LOG_WARNING, "can not determine buffering position");
    return;
  }

  buffered_position_ = cached_position;
#if 0
  av_log(nullptr,
         AV_LOG_INFO,
         "video_pkt_queue: packets number = %d, duration = %lf s , size = %d\n",
         video_pkt_queue->nb_packets,
         video_pkt_queue->duration * av_q2d(video_pkt_queue->time_base),
         video_pkt_queue->size);
  video_render_->DumpDebugInformation();
#endif

  if (check_queue_is_ready(video_pkt_queue, data_source->ContainVideoStream())
      && check_queue_is_ready(audio_pkt_queue, data_source->ContainAudioStream())) {
    ChangePlaybackState(MediaPlayerState::READY);
    if (play_when_ready_) {
//      StartRenders();
    }
  }

}
void MediaPlayer::OnFirstFrameLoaded(int width, int height) {
  if (on_video_size_changed_) {
    on_video_size_changed_(width, height);
  }
}

void MediaPlayer::OnFirstFrameRendered(int width, int height) {
  if (on_video_size_changed_) {
    on_video_size_changed_(width, height);
  }

}

}
