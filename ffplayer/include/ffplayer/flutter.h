//
// Created by boyan on 2021/1/24.
//

#ifndef FF_PLAYER_FLUTTER_H
#define FF_PLAYER_FLUTTER_H

#ifdef __cplusplus
extern "C" {
#endif

void flutter_on_post_player_created(void* player);

void flutter_on_pre_player_free(void* player);

void flutter_free_all_player(void (*free_handle)(void* player));

#ifdef __cplusplus
}
#endif

#endif //FF_PLAYER_FLUTTER_H
