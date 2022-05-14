//
// Created by Bin Yang on 2022/5/14.
//
#include <iostream>
#include <thread>

#include <lychee_player.h>
#include "base/task_runner.h"

namespace {

void printPlayerState(
    const lychee::LycheePlayer *player,
    media::TaskRunner &task_runner
) {
  std::cout << "player: " << player->CurrentTime() << "/" << player->GetDuration() << std::endl;
  task_runner.PostDelayedTask(
      FROM_HERE,
      media::TimeDelta::FromSeconds(1),
      [player, &task_runner]() {
        printPlayerState(player, task_runner);
      }
  );
}

}
int main() {
  lychee::LycheePlayer::GlobalInit();

  auto message_loop = media::base::MessageLooper::Create("main");
  message_loop->Prepare();
  auto task_runner = media::TaskRunner(message_loop);

  auto player = std::make_unique<lychee::LycheePlayer>("/Users/yangbin/Music/网易云音乐/来碗鱼汤吗 - 石头歌（吉他版）.mp3");
  player->SetPlayerStateChangedCallback([](lychee::LycheePlayer::PlayerState state) {
    std::cout << "player state changed: " << state << std::endl;
    if (state == lychee::LycheePlayer::kPlayerEnded) {
      std::cout << "player ended" << std::endl;
      exit(0);
    }
  });

  task_runner.PostTask(FROM_HERE, [&player]() {
    player->SetPlayWhenReady(true);
  });

  printPlayerState(player.get(), task_runner);

  message_loop->Loop();
  return 0;
}