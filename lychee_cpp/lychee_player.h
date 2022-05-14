//
// Created by boyan on 22-5-3.
//

#ifndef LYCHEE_CPP__LYCHEE_PLAYER_H_
#define LYCHEE_CPP__LYCHEE_PLAYER_H_

#include <string>
#include <memory>

#include "demuxer.h"
#include "audio_decoder.h"
#include "audio_renderer.h"

namespace lychee {

class LycheePlayer;

class PlayerCallback {
 public:
  virtual void OnPlayerEnded(const std::unique_ptr<LycheePlayer> &player) = 0;
  virtual void OnBuffering(const std::unique_ptr<LycheePlayer> &player, double progress) = 0;

  virtual ~PlayerCallback() = 0;

};

class LycheePlayer : public DemuxerHost, public AudioRendererHost {

 public:

  static void GlobalInit();

  explicit LycheePlayer(const std::string &path);

  ~LycheePlayer();

  void Seek(double seconds);

  void SetPlayWhenReady(bool play_when_ready);

  void Stop();

  void SetPlayerCallback(std::unique_ptr<PlayerCallback> callback);

  void OnDemuxerHasEnoughData() override;

  void OnDemuxerBuffering(double progress) override;

  void OnAudioRendererEnded() override;
  void OnAudioRendererNeedMoreData() override;

 private:
  std::unique_ptr<PlayerCallback> callback_;

  std::unique_ptr<Demuxer> demuxer_;
  std::unique_ptr<AudioDecoder> audio_decoder_;
  std::shared_ptr<AudioRenderer> audio_renderer_;

  media::TaskRunner task_runner_;

  bool initialized_;

  bool play_when_ready_;

  void Initialize();
};

} // lychee

#endif //LYCHEE_CPP__LYCHEE_PLAYER_H_
