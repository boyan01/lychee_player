//
// Created by boyan on 2021/1/24.
//

#ifndef FF_PLAYER_FLUTTER_H
#define FF_PLAYER_FLUTTER_H

#ifdef _FLUTTER_WINDOWS
#include <flutter/plugin_registrar_windows.h>
#endif

#include "proto.h"

void flutter_on_post_player_created(CPlayer *player);

void flutter_on_pre_player_free(CPlayer *player);

void flutter_free_all_player(void (*free_handle)(CPlayer *player));


#endif //FF_PLAYER_FLUTTER_H
