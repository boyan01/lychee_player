//
// Created by yangbin on 2021/2/13.
//

#include <base/bind_to_current_loop.h>
#include "base/logging.h"
#include "base/lambda.h"

#include "media_player.h"

#include "ffp_define.h"
#include "ffp_utils.h"

extern "C" {
#include "libavutil/bprint.h"
}

namespace media {

MediaPlayer::MediaPlayer(
    std::unique_ptr<VideoRendererSink> video_renderer_sink,
    std::shared_ptr<AudioRendererSink> audio_renderer_sink,
    const TaskRunner &task_runner
) : task_runner_(task_runner) {
  task_runner_.PostTask(FROM_HERE, [&]() {
    Initialize();
  });
  auto decoder_looper = MessageLooper::PrepareLooper("audio_decoder");
  auto decoder_task_runner = std::make_shared<TaskRunner>(decoder_looper);
  audio_renderer_ = std::make_shared<AudioRenderer>(decoder_task_runner, std::move(audio_renderer_sink));
  video_renderer_ = std::make_shared<VideoRenderer>(
      task_runner_,
      std::make_shared<TaskRunner>(MessageLooper::PrepareLooper("video_decoder")),
      std::move(video_renderer_sink));
}

void MediaPlayer::Initialize() {
  DCHECK_EQ(state_, kUninitialized);
  DCHECK(task_runner_.BelongsToCurrentThread());

  auto sync_type_confirm = [this](int av_sync_type) -> int {
    return AV_SYNC_AUDIO_MASTER;
//    if (data_source == nullptr) {
//      return av_sync_type;
//    }
//    if (av_sync_type == AV_SYNC_VIDEO_MASTER) {
//      if (data_source->ContainVideoStream()) {
//        return AV_SYNC_VIDEO_MASTER;
//      } else {
//        return AV_SYNC_AUDIO_MASTER;
//      }
//    } else if (av_sync_type == AV_SYNC_AUDIO_MASTER) {
//      if (data_source->ContainAudioStream()) {
//        return AV_SYNC_AUDIO_MASTER;
//      } else {
//        return AV_SYNC_EXTERNAL_CLOCK;
//      }
//    } else {
//      return AV_SYNC_EXTERNAL_CLOCK;
//    }
  };
  clock_context = std::make_shared<MediaClock>(nullptr, nullptr,
                                               sync_type_confirm);
  task_runner_.PostTask(FROM_HERE, std::bind(&MediaPlayer::DumpMediaClockStatus, this));

  state_ = kIdle;

}

MediaPlayer::~MediaPlayer() {
  std::lock_guard<std::mutex> lock_guard(player_mutex_);
  DLOG(INFO) << "Destroy Media Player " << this;
  task_runner_ = nullptr;
  demuxer_ = nullptr;
  audio_renderer_ = nullptr;
  video_renderer_ = nullptr;
}

void MediaPlayer::SetPlayWhenReady(bool play_when_ready) {
  std::lock_guard<std::mutex> lock_guard(player_mutex_);
  if (play_when_ready_pending_ == play_when_ready) {
    return;
  }
  play_when_ready_pending_ = play_when_ready;
  task_runner_.PostTask(FROM_HERE, [&]() {
    SetPlayWhenReadyTask(play_when_ready_pending_);
  });
}

void MediaPlayer::SetPlayWhenReadyTask(bool play_when_ready) {
  if (play_when_ready_ == play_when_ready) {
    return;
  }
  play_when_ready_ = play_when_ready;
  if (state_ != kPrepared) {
    return;
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
    if (video_renderer_) {
//      video_renderer_->frame_timer_ += get_relative_time() - clock_context->GetVideoClock()->last_updated;
    }
//    if (data_source && data_source->read_pause_return != AVERROR(ENOSYS)) {
//      clock_context->GetVideoClock()->paused = 0;
//    }
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
  if (demuxer_) {
    av_log(nullptr, AV_LOG_ERROR, "can not open file multi-times.\n");
    return -1;
  }
  task_runner_.PostTask(FROM_HERE, [&, filename]() {
    OpenDataSourceTask(filename);
  });
  return 0;
}

void MediaPlayer::OpenDataSourceTask(const char *filename) {
  DCHECK(task_runner_.BelongsToCurrentThread());
  DCHECK_EQ(state_, kIdle);

  DLOG(INFO) << "open file: " << filename;
  state_ = kPreparing;
  demuxer_ = std::make_shared<Demuxer>(TaskRunner(MessageLooper::PrepareLooper("demux")), filename,
                                       [](std::unique_ptr<MediaTracks> tracks) {
                                         DLOG(INFO) << "on tracks update.";
                                         for (auto &track: tracks->tracks()) {
                                           DLOG(INFO) << "track: " << *track;
                                         }
                                       });
  demuxer_->Initialize(this, bind_weak(&MediaPlayer::OnDataSourceOpen, shared_from_this()));
}

void MediaPlayer::OnDataSourceOpen(int open_status) {
  DCHECK_EQ(state_, kPreparing);
  if (open_status >= 0) {
    DLOG(INFO) << "Open DataSource Succeed";
    task_runner_.PostTask(FROM_HERE, bind_weak(&MediaPlayer::InitVideoRender, shared_from_this()));
  } else {
    state_ = kIdle;
    DLOG(ERROR) << "Open DataSource Failed";
  }
}

void MediaPlayer::InitVideoRender() {
  auto stream = demuxer_->GetFirstStream(DemuxerStream::Video);
  if (stream) {
    video_renderer_->Initialize(stream,
                                clock_context,
                                bind_weak(&MediaPlayer::OnVideoRendererInitialized, shared_from_this()));
  } else {
    DLOG(WARNING) << "data source does not contains video stream";
    task_runner_.PostTask(FROM_HERE, bind_weak(&MediaPlayer::InitAudioRender, shared_from_this()));
  }
}

void MediaPlayer::OnVideoRendererInitialized(bool success) {
  if (!success) {
    state_ = kIdle;
    return;
  }
  task_runner_.PostTask(FROM_HERE, bind_weak(&MediaPlayer::InitAudioRender, shared_from_this()));
}

void MediaPlayer::InitAudioRender() {
  auto stream = demuxer_->GetFirstStream(DemuxerStream::Audio);
  if (stream) {
    audio_renderer_->Initialize(stream, clock_context,
                                bind_weak(&MediaPlayer::OnAudioRendererInitialized, shared_from_this()));
  }
}

void MediaPlayer::OnAudioRendererInitialized(bool success) {
  DLOG(INFO) << __func__ << " : " << success;
  if (success) {
    state_ = kPrepared;
    if (play_when_ready_) {
      StartRenders();
    }
  } else {
    state_ = kIdle;
  }
}

void MediaPlayer::DumpStatus() {

}

TimeDelta MediaPlayer::GetCurrentPosition() {
  if (state_ == kUninitialized) {
    return TimeDelta::Zero();
  }

  double position = clock_context->GetMasterClock();
  if (isnan(position)) {
    if (demuxer_ && demuxer_->GetPendingSeekingPosition().is_positive()) {
      return demuxer_->GetPendingSeekingPosition();
    } else {
      return TimeDelta::Zero();
    }
  }
  return TimeDelta::FromSecondsD(position);
}

double MediaPlayer::GetVolume() {
  if (!audio_renderer_) {
    return 0;
  }
  return audio_renderer_->GetVolume();
}

void MediaPlayer::SetVolume(double volume) {
  if (!audio_renderer_) {
    return;
  }
  DLOG_IF(WARNING, volume > 1 || volume < 0) << "volume is out of range [0, 1] : " << volume;
  if (volume < 0) {
    volume = 0;
  }
  if (volume > 1) {
    volume = 1;
  }
  audio_renderer_->SetVolume(volume);
}

double MediaPlayer::GetDuration() const {
  return duration_;
}

void MediaPlayer::Seek(TimeDelta position) {
  if (position < TimeDelta::Zero()) {
    DLOG(WARNING) << "invalid seek position: " << position.InSecondsF();
    return;
  }
  task_runner_.PostTask(FROM_HERE, [&, position]() {
    ChangePlaybackState(MediaPlayerState::BUFFERING);
    demuxer_->AbortPendingReads();
    demuxer_->SeekTo(position, BindToCurrentLoop(bind_weak(&MediaPlayer::OnSeekCompleted, shared_from_this())));
  });
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

void MediaPlayer::ChangePlaybackState(MediaPlayerState state) {
  if (player_state_ == state) {
    return;
  }
  player_state_ = state;
}

void MediaPlayer::StopRenders() {
  DLOG(INFO) << "StopRenders";
  PauseClock(true);
  if (audio_renderer_) {
    audio_renderer_->Stop();
  }
  if (video_renderer_) {
    video_renderer_->Stop();
  }
}

void MediaPlayer::StartRenders() {
  DLOG(INFO) << "StartRenders";
  PauseClock(false);
  if (audio_renderer_) {
    audio_renderer_->Start();
  }
  // TODO fixme
  if (video_renderer_ && demuxer_->GetFirstStream(DemuxerStream::Video)) {
    video_renderer_->Start();
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

void MediaPlayer::DumpMediaClockStatus() {
//
//  DLOG(INFO) << "DumpMediaClockStatus: master clock = "
//             << clock_context->GetMasterClock();

#if 0
  if (audio_renderer_) {
    DLOG(INFO) << "AudioRenderer" << *audio_renderer_;
  }
  if (video_renderer_) {
    DLOG(INFO) << "VideoRenderer" << *video_renderer_;
  }
  if (demuxer_) {
    auto audio_stream = demuxer_->GetFirstStream(DemuxerStream::Audio);
    if (audio_stream) {
      DLOG(INFO) << " audio_stream: " << *audio_stream;
    }
    auto video_stream = demuxer_->GetFirstStream(DemuxerStream::Video);
    if (video_stream) {
      DLOG(INFO) << " video_stream: " << *video_stream;
    }
  }

  decode_task_runner_.PostDelayedTask(FROM_HERE, TimeDelta(1000000), std::bind(&MediaPlayer::DumpMediaClockStatus, this));
#endif
}

void MediaPlayer::SetDuration(double duration) {
  duration_ = duration;
}

void MediaPlayer::OnDemuxerError(PipelineStatus error) {

}

void MediaPlayer::OnSeekCompleted(bool succeed) {
  DLOG(INFO) << "OnSeekCompleted: " << succeed;
  if (!succeed) {
    return;
  }
  if (audio_renderer_) {
    audio_renderer_->Flush();
  }
  if (video_renderer_) {
    video_renderer_->Flush();
  }
}

}
