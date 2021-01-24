//
// Created by boyan on 2021/1/24.
//

#ifdef _FLUTTER

#include "ffplayer/flutter.h"
#include <list>
#include <iostream>


static std::list<void *> players;

void flutter_on_post_player_created(void *player) {
    players.push_back(player);
}

void flutter_on_pre_player_free(void *player) {
    players.remove(player);
}

void flutter_free_all_player(void (*free_handle)(void *)) {
    for (auto player : players) {
        free_handle(player);
    }
    players.clear();
}


#endif