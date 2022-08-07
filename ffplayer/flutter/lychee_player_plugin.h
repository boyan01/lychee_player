//
// Created by Bin Yang on 2022/5/14.
//

#ifndef LYCHEE_PLAYER_FLUTTER_LYCHEE_PLAYER_PLUGIN_H_
#define LYCHEE_PLAYER_FLUTTER_LYCHEE_PLAYER_PLUGIN_H_

#if _WIN32
#include <Windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#include "stdint.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

#if _WIN32
#define FFI_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FFI_PLUGIN_EXPORT
#endif

FFI_PLUGIN_EXPORT void* lychee_player_create(const char* file_path,
                                             int64_t send_port);

FFI_PLUGIN_EXPORT void lychee_player_dispose(void* player);

FFI_PLUGIN_EXPORT void lychee_player_set_play_when_ready(void* player,
                                                         bool play_when_ready);

FFI_PLUGIN_EXPORT void lychee_player_seek(void* player, double time);

FFI_PLUGIN_EXPORT double lychee_player_get_current_time(void* player);

FFI_PLUGIN_EXPORT double lychee_player_get_duration(void* player);

FFI_PLUGIN_EXPORT void lychee_player_initialize_dart(void* native_port);

#ifdef __cplusplus
}
#endif

#endif  // LYCHEE_PLAYER_FLUTTER_LYCHEE_PLAYER_PLUGIN_H_
