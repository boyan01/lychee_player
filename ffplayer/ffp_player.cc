//
// Created by yangbin on 2021/2/13.
//

#include "ffp_player.h"

extern "C" {
#include "libavutil/bprint.h"
}

extern AVPacket *flush_pkt;

CPlayer::CPlayer() {
    message_context = std::make_shared<MessageContext>();

    audio_pkt_queue = std::make_shared<PacketQueue>();
    video_pkt_queue = std::make_shared<PacketQueue>();
    subtitle_pkt_queue = std::make_shared<PacketQueue>();

    auto sync_type_confirm = [this](int av_sync_type) -> int {
        if (data_source == nullptr) {
            return av_sync_type;
        }
        if (av_sync_type == AV_SYNC_VIDEO_MASTER) {
            if (data_source->ContainVideoStream()) {
                return AV_SYNC_VIDEO_MASTER;
            } else {
                return AV_SYNC_AUDIO_MASTER;
            }
        } else if (av_sync_type == AV_SYNC_AUDIO_MASTER) {
            if (data_source->ContainAudioStream()) {
                return AV_SYNC_AUDIO_MASTER;
            } else {
                return AV_SYNC_EXTERNAL_CLOCK;
            }
        } else {
            return AV_SYNC_EXTERNAL_CLOCK;
        }
    };
    clock_context = std::make_shared<ClockContext>(&audio_pkt_queue->serial, &video_pkt_queue->serial,
                                                   sync_type_confirm);

    audio_render = std::make_shared<AudioRender>(audio_pkt_queue, clock_context);
    video_render = std::make_shared<VideoRender>(video_pkt_queue, clock_context, message_context);

    decoder_context = std::make_unique<DecoderContext>(audio_render, video_render, clock_context);
}

CPlayer::~CPlayer() {
}

void CPlayer::TogglePause() {
    if (paused) {
        video_render->frame_timer +=
                av_gettime_relative() / 1000000.0 - clock_context->GetVideoClock()->last_updated;
        if (data_source->read_pause_return != AVERROR(ENOSYS)) {
            clock_context->GetVideoClock()->paused = 0;
        }
        clock_context->GetVideoClock()->SetClock(clock_context->GetVideoClock()->GetClock(),
                                                 clock_context->GetVideoClock()->serial);
    }
    clock_context->GetExtClock()->SetClock(clock_context->GetExtClock()->GetClock(),
                                           clock_context->GetExtClock()->serial);

    paused = !paused;
    clock_context->GetExtClock()->paused = clock_context->GetAudioClock()->paused
            = clock_context->GetVideoClock()->paused = paused;
    if (data_source) {
        data_source->paused = paused;
    }
    audio_render->paused = paused;
    video_render->paused_ = paused;
    video_render->step = false;
}

int CPlayer::OpenDataSource(const char *filename) {
    if (data_source) {
        av_log(nullptr, AV_LOG_ERROR, "can not open file multi-times.\n");
        return -1;
    }

    data_source = std::make_unique<DataSource>(filename, nullptr);
    data_source->audio_queue = audio_pkt_queue;
    data_source->video_queue = video_pkt_queue;
    data_source->subtitle_queue = subtitle_pkt_queue;
    data_source->ext_clock = clock_context->GetAudioClock();
    data_source->decoder_ctx = decoder_context.get();
    data_source->msg_ctx = message_context.get();

    data_source->Open();
    return 0;
}

void CPlayer::DumpStatus() {
    AVBPrint buf;
    static int64_t last_time;
    int64_t cur_time;
    int aqsize, vqsize, sqsize;
    double av_diff;

    cur_time = av_gettime_relative();
    if (!last_time || (cur_time - last_time) >= 30000) {
        aqsize = 0;
        vqsize = 0;
        sqsize = 0;
        if (data_source->ContainAudioStream())
            aqsize = audio_pkt_queue->size;
        if (data_source->ContainVideoStream())
            vqsize = video_pkt_queue->size;
        if (data_source->ContainSubtitleStream())
            sqsize = subtitle_pkt_queue->size;
        av_diff = 0;
        if (data_source->ContainAudioStream() && data_source->ContainVideoStream())
            av_diff = clock_context->GetAudioClock()->GetClock() -
                      clock_context->GetVideoClock()->GetClock();
        else if (data_source->ContainVideoStream())
            av_diff = clock_context->GetMasterClock() - clock_context->GetVideoClock()->GetClock();
        else if (data_source->ContainAudioStream())
            av_diff = clock_context->GetMasterClock() - clock_context->GetAudioClock()->GetClock();

        av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
        av_bprintf(&buf,
                   "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"
                   PRId64
                   "/%"
                   PRId64
                   "   \r",
                   clock_context->GetMasterClock(),
                   (data_source->ContainAudioStream()
                    && data_source->ContainAudioStream()) ? "A-V"
                                                          : (data_source->ContainVideoStream()
                                                             ? "M-V"
                                                             : (data_source->ContainAudioStream()
                                                                ? "M-A"
                                                                : "   ")),
                   av_diff,
                   video_render->frame_drop_count + decoder_context->frame_drop_count,
                   aqsize / 1024,
                   vqsize / 1024,
                   sqsize,
                /*data_source->ContainVideoStream() ? decoder_context->.avctx->pts_correction_num_faulty_dts :*/
                   0ll,
                /* is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts :*/ 0ll);

        if (show_status == 1 && AV_LOG_INFO > av_log_get_level())
            fprintf(stderr, "%s", buf.str);
        else
            av_log(nullptr, AV_LOG_INFO, "%s", buf.str);

        fflush(stderr);
        av_bprint_finalize(&buf, nullptr);

        last_time = cur_time;
    }

}

double CPlayer::GetCurrentPosition() {
    double position = clock_context->GetMasterClock();
    if (isnan(position)) {
        if (data_source) {
            position = (double) data_source->GetSeekPosition() / AV_TIME_BASE;
        } else {
            position = 0;
        }
    }
    return position;
}

bool CPlayer::IsPaused() const {
    return paused;
}

int CPlayer::GetVolume() {
    return audio_render->GetVolume();
}

void CPlayer::SetVolume(int volume) {
    audio_render->SetVolume(volume);
}

void CPlayer::SetMute(bool mute) {
    audio_render->SetMute(mute);
}

bool CPlayer::IsMuted() {
    return audio_render->IsMute();
}

double CPlayer::GetDuration() {
    CHECK_VALUE_WITH_RETURN(data_source, -1);
    return data_source->GetDuration();
}

void CPlayer::Seek(double position) {
    CHECK_VALUE(data_source);
    data_source->Seek(position);
}

void CPlayer::SeekToChapter(int chapter) {
    CHECK_VALUE(data_source);
    data_source->SeekToChapter(chapter);
}

int CPlayer::GetCurrentChapter() {
    CHECK_VALUE_WITH_RETURN(data_source, -1);
    int64_t pos = GetCurrentPosition() * AV_TIME_BASE;
    return data_source->GetChapterByPosition(pos);
}

int CPlayer::GetChapterCount() {
    CHECK_VALUE_WITH_RETURN(data_source, -1);
    return data_source->GetChapterCount();
}

void CPlayer::SetMessageHandleCallback(std::function<void(int what, int64_t arg1, int64_t arg2)> message_callback) {
    message_context->message_callback = std::move(message_callback);
}

double CPlayer::GetVideoAspectRatio() {
    return video_render->GetVideoAspectRatio();
}

const char *CPlayer::GetUrl() {
    CHECK_VALUE_WITH_RETURN(data_source, nullptr);
    return data_source->GetFileName();
}

const char *CPlayer::GetMetadataDict(const char *key) {
    CHECK_VALUE_WITH_RETURN(data_source, nullptr);
    return data_source->GetMetadataDict(key);
}

void CPlayer::GlobalInit() {
    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_level(AV_LOG_INFO);
    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
        av_log(nullptr, AV_LOG_ERROR, "SDL fails to initialize audio subsystem!\n%s", SDL_GetError());
    else
        av_log(nullptr, AV_LOG_DEBUG, "SDL Audio was initialized fine!\n");

    flush_pkt = new AVPacket;
    av_init_packet(flush_pkt);
    flush_pkt->data = (uint8_t *) &flush_pkt;

}

void CPlayer::SetVideoRender(unique_ptr_d<FFP_VideoRenderCallback> render_callback) {
    CHECK_VALUE(render_callback);
    video_render->render_callback = std::move(render_callback);
    if (video_render->Start()) {
        video_render->render_attached = true;
    }
}

void CPlayer::DrawFrame() {
    video_render->DrawFrame();
}

