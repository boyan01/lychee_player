//
// Created by boyan on 22-5-3.
//

#include "lychee_player.h"

extern "C" {
#include "SDL2/SDL.h"
#include "libavutil/log.h"
#include "libavdevice/avdevice.h"
#include "libavformat/avformat.h"
}

#include <base/logging.h>

#include <utility>

#include "sdl_audio_renderer.h"

namespace lychee {

void LycheePlayer::GlobalInit() {
  av_log_set_flags(AV_LOG_SKIP_REPEATED);
  av_log_set_level(AV_LOG_INFO);
  avformat_network_init();

  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    LOG(ERROR) << "SDL fails to initialize audio subsystem! \n" << SDL_GetError();
  } else {
    DLOG(INFO) << "SDL Audio was initialized fine!";
  }
}

LycheePlayer::LycheePlayer(
    const std::string &path
) : demuxer_(), initialized_(false),
    play_when_ready_(false),
    source_state_(SourceState::kSourceUninitialized),
    duration_(0), player_state_changed_callback_(nullptr),
    player_state_(kPlayerBuffering) {
  DLOG(INFO) << "create player: " << path;
  auto media_message_loop = media::MessageLooper::PrepareLooper("media_task_runner");
  task_runner_ = media::TaskRunner(media_message_loop);
  auto task_runner = media::TaskRunner(media_message_loop);
  demuxer_ = std::make_unique<Demuxer>(task_runner, path, this);
  audio_decoder_ = std::make_unique<AudioDecoder>(task_runner);
  audio_renderer_ = std::make_shared<SdlAudioRenderer>(task_runner, this);

  task_runner_.PostTask(FROM_HERE, [this]() { Initialize(); });

}

void LycheePlayer::Initialize() {
  DCHECK(task_runner_.BelongsToCurrentThread());
  DLOG(INFO) << "initialize player";
  demuxer_->Initialize([this](const std::shared_ptr<DemuxerStream> &stream) {
    DCHECK(stream) << "stream is null";
    DLOG(INFO) << "stream is ready";
    audio_renderer_->Initialize(
        stream->audio_decode_config(),
        [this, stream(stream)](AudioDeviceInfo *audio_device_info) {
          if (!audio_device_info) {
            DLOG(ERROR) << "failed to open audio device";
            return;
          }
          audio_decoder_->Initialize(stream, *audio_device_info, [this](std::shared_ptr<AudioBuffer> buffer) {
            audio_renderer_->OnNewFrameAvailable(std::move(buffer));
          }, [this](bool initialized) {
            DLOG(INFO) << "audio decoder initialized";
            initialized_ = true;
            demuxer_->NotifyCapacityAvailable();
          });
        });

  });
}

void LycheePlayer::SetPlayWhenReady(bool play_when_ready) {
  task_runner_.PostTask(FROM_HERE, [this, play_when_ready(play_when_ready)]() {
    play_when_ready_ = play_when_ready;
    if (!initialized_) {
      return;
    }
    if (play_when_ready) {
      audio_renderer_->Play();
    } else {
      audio_renderer_->Pause();
    }
  });
}

void LycheePlayer::OnDemuxerHasEnoughData() {
  task_runner_.PostTask(FROM_HERE, [this]() {
    DLOG_IF(INFO, source_state_ != kSourceBuffering && source_state_ != kSourceReady)
    << "source state: " << source_state_;
    source_state_ = SourceState::kSourceReady;
    ChangePlayerState(kPlayerReady);
    if (play_when_ready_) {
      audio_renderer_->Play();
    }
  });
}

void LycheePlayer::OnAudioRendererEnded() {
  task_runner_.PostTask(FROM_HERE, [this]() {
    ChangePlayerState(kPlayerEnded);
  });
}

void LycheePlayer::OnAudioRendererNeedMoreData() {
  audio_decoder_->PostDecodeTask();
}

void LycheePlayer::Seek(double seconds) {

}

void LycheePlayer::Stop() {

}

void LycheePlayer::SetPlayerCallback(std::unique_ptr<PlayerCallback> callback) {

}

void LycheePlayer::OnDemuxerBuffering(double progress) {
  source_state_ = kSourceBuffering;
  ChangePlayerState(kPlayerBuffering);
}

double LycheePlayer::GetDuration() const {
  return duration_;
}

double LycheePlayer::CurrentTime() const {
  return audio_renderer_->CurrentTime();
}

void LycheePlayer::SetDuration(double duration) {
  task_runner_.PostTask(FROM_HERE, [this, duration]() {
    duration_ = duration;
    LOG_IF(WARNING, duration_ < 0) << "duration is negative";
    DLOG_IF(WARNING, source_state_ != SourceState::kSourceUninitialized)
    << "duration is set after source state is not uninitialized";
    source_state_ = SourceState::kSourceBuffering;
    ChangePlayerState(kPlayerBuffering);
  });
}

void LycheePlayer::SetPlayerStateChangedCallback(LycheePlayer::PlayerStateChangedCallback callback) {
  player_state_changed_callback_ = std::move(callback);
}

void LycheePlayer::ChangePlayerState(LycheePlayer::PlayerState state) {
  if (player_state_ == state) {
    return;
  }
  player_state_ = state;
  if (player_state_changed_callback_) {
    player_state_changed_callback_(state);
  }
}

LycheePlayer::PlayerState LycheePlayer::GetPlayerState() const {
  return player_state_;
}

LycheePlayer::~LycheePlayer() = default;

} // lychee