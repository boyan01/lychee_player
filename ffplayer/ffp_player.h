//
// Created by yangbin on 2021/2/13.
//

#ifndef FFPLAYER_FFP_PLAYER_H
#define FFPLAYER_FFP_PLAYER_H

#include <memory>
#include <functional>

#include "ffplayer.h"
#include "ffp_packet_queue.h"
#include "ffp_clock.h"
#include "ffp_data_source.h"
#include "ffp_audio_render.h"
#include "ffp_video_render.h"

typedef enum FFPlayerState_ {
    FFP_STATE_IDLE = 0,
    FFP_STATE_READY,
    FFP_STATE_BUFFERING,
    FFP_STATE_END
} FFPlayerState;

struct CPlayer {

private:

    const std::unique_ptr<PacketQueue> audio_pkt_queue = std::unique_ptr<PacketQueue>(new PacketQueue);
    const std::unique_ptr<PacketQueue> video_pkt_queue = std::unique_ptr<PacketQueue>(new PacketQueue);
    const std::unique_ptr<PacketQueue> subtitle_pkt_queue = std::unique_ptr<PacketQueue>(new PacketQueue);

    const std::unique_ptr<ClockContext> clock_context = std::unique_ptr<ClockContext>(new ClockContext);

    DataSource *data_source = nullptr;

    const std::unique_ptr<DecoderContext> decoder_context = std::unique_ptr<DecoderContext>(new DecoderContext);

    const std::unique_ptr<AudioRender> audio_render = std::unique_ptr<AudioRender>(new AudioRender);
    const std::unique_ptr<VideoRender> video_render = std::unique_ptr<VideoRender>(new VideoRender);

    const std::shared_ptr<MessageContext> message_context = std::make_shared<MessageContext>();

    void DumpStatus();

public:

    CPlayer();

    ~CPlayer();

    static void GlobalInit();


public:
    PlayerConfiguration start_configuration{};

    int show_status = -1;

    // buffered position in seconds. -1 if not avalible
    int64_t buffered_position = -1;

    SDL_Thread *msg_tid = nullptr;
    FFPlayerState state = FFP_STATE_IDLE;

    bool paused = false;

    void TogglePause();

    int OpenDataSource(const char *filename);

    double GetCurrentPosition();

    bool IsPaused() const;

    int GetVolume();

    void SetVolume(int volume);

    void SetMute(bool mute);

    bool IsMuted();

    double GetDuration();

    void Seek(double position);

    void SeekToChapter(int chapter);

    int GetCurrentChapter();

    int GetChapterCount();

    void SetMessageHandleCallback(std::function<void(int what, int64_t arg1, int64_t arg2)> message_callback);

    double GetVideoAspectRatio();

    const char *GetUrl();

    const char *GetMetadataDict(const char *key);

    void SetVideoRender(unique_ptr_d<FFP_VideoRenderCallback> render_callback);

    void DrawFrame();
};


#endif //FFPLAYER_FFP_PLAYER_H
