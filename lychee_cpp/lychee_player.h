//
// Created by boyan on 22-5-3.
//

#ifndef LYCHEE_CPP__LYCHEE_PLAYER_H_
#define LYCHEE_CPP__LYCHEE_PLAYER_H_

#include <string>
#include <memory>

namespace lychee {

class LycheePlayer;

class PlayerCallback {
 public:
  virtual void OnPlayerEnded(const std::unique_ptr<LycheePlayer> &player) = 0;
  virtual void OnBuffering(const std::unique_ptr<LycheePlayer> &player, double progress) = 0;

  virtual ~PlayerCallback() = 0;

};

class LycheePlayer {

 public:

  static void GlobalInit();

  explicit LycheePlayer(std::string path);

  ~LycheePlayer();

  void Seek(double seconds);

  void SetPlayWhenReady(bool play_when_ready);

  void Stop();

  void SetPlayerCallback(std::unique_ptr<PlayerCallback> callback);

 private:
  std::unique_ptr<PlayerCallback> callback_;
};

} // lychee

#endif //LYCHEE_CPP__LYCHEE_PLAYER_H_
