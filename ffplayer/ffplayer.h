#ifndef FFPLAYER_FFPLAYER_H_
#define FFPLAYER_FFPLAYER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"

struct CPlayer;

typedef struct CPlayer CPlayer;

#ifdef _WIN32
#define FFPLAYER_EXPORT  __declspec(dllexport)
#else
#define FFPLAYER_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif


struct PlayerConfiguration {
    int32_t audio_disable = false;
    int32_t video_disable = false;
    int32_t subtitle_disable = false;

    int32_t seek_by_bytes = false;

    int32_t show_status = true;

    int64_t start_time = 0;
    int32_t loop = 1;
};

#ifdef __cplusplus
};
#endif

#endif  // FFPLAYER_FFPLAYER_H_