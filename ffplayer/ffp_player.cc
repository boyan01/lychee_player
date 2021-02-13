//
// Created by yangbin on 2021/2/13.
//

#include "ffp_player.h"

#include <utility>

extern "C" {
#include "libavutil/bprint.h"
}

CPlayer::CPlayer() {
    clock_context->Init(&audio_pkt_queue->serial,
                        &video_pkt_queue->serial,
                        [this](int av_sync_type) -> int {
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
                        });

    message_context->Start();

    audio_render->Init(audio_pkt_queue.get(), clock_context.get());
    video_render->Init(video_pkt_queue.get(), clock_context.get(), message_context);
    decoder_context->audio_render = audio_render.get();
    decoder_context->video_render = video_render.get();
    decoder_context->clock_ctx = clock_context.get();
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

int CPlayer::OpenDataSource(const char *_filename) {
    if (data_source) {
        av_log(nullptr, AV_LOG_ERROR, "can not open file multi-times.\n");
        return -1;
    }

    data_source = new DataSource(_filename, nullptr);
    data_source->audio_queue = audio_pkt_queue.get();
    data_source->video_queue = video_pkt_queue.get();
    data_source->subtitle_queue = subtitle_pkt_queue.get();
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
