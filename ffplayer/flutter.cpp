//
// Created by boyan on 2021/1/24.
//

#ifdef _FLUTTER

#include "ffplayer/flutter.h"
#include <list>


static std::list<CPlayer *> players_;

void flutter_on_post_player_created(CPlayer *player) {
    players_.push_back(player);
}


void flutter_on_pre_player_free(CPlayer *player) {
    players_.remove(player);
}

void flutter_free_all_player(void (*free_handle)(CPlayer *)) {
    for (auto player : players_) {
        free_handle(player);
    }
    players_.clear();
}


#endif