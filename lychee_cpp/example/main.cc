//
// Created by Bin Yang on 2022/5/14.
//
#include <iostream>
#include <thread>

#include <lychee_player.h>

int main() {
  lychee::LycheePlayer::GlobalInit();

  auto player = std::make_unique<lychee::LycheePlayer>("/Users/yangbin/Music/网易云音乐/来碗鱼汤吗 - 石头歌（吉他版）.mp3");
  player->SetPlayWhenReady(true);

  std::this_thread::sleep_for(std::chrono::seconds(60));

  return 0;
}