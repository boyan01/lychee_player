//
// Created by boyan on 2021/2/10.
//

#include "ffplayer/ffplayer.h"
#include "ffp_data_source.h"

extern "C" {
#include "libavutil/rational.h"
}

char av_error[AV_ERROR_MAX_STRING_SIZE] = {0};
#define av_err2str(errnum) av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)

extern AVPacket *flush_pkt;


static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

static int is_realtime(AVFormatContext *s) {
    if (!strcmp(s->iformat->name, "rtp") || !strcmp(s->iformat->name, "rtsp") || !strcmp(s->iformat->name, "sdp"))
        return 1;

    if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4)))
        return 1;
    return 0;
}


/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg) {
    auto *player = static_cast<CPlayer *>(arg);
    auto *is = player->is;
    AVFormatContext *ic = nullptr;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;

    if (!wait_mutex) {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    ic = avformat_alloc_context();
    if (!ic) {
        av_log(nullptr, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = [](void *ctx) -> int {
        auto *is = static_cast<VideoState *>(ctx);
        return is->abort_request;
    };
    ic->interrupt_callback.opaque = is;

    err = avformat_open_input(&ic, is->filename, is->iformat, nullptr);
    if (err < 0) {
        // print_error(is->filename, err);
//        av_log(nullptrptr, AV_LOG_ERROR, "can not open file %s: %s", is->filename, av_err2str(err));
        ret = -1;
        goto fail;
    }

    is->ic = ic;

    if (player->genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    av_format_inject_global_side_data(ic);

    if (player->find_stream_info) {
        unsigned int orig_nb_streams = ic->nb_streams;

        err = avformat_find_stream_info(ic, nullptr);
        if (err < 0) {
            av_log(nullptr, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }

    if (ic->pb)
        ic->pb->eof_reached = 0;  // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (player->seek_by_bytes < 0)
        player->seek_by_bytes = (ic->iformat->flags & AVFMT_TS_DISCONT) != 0 && strcmp("ogg", ic->iformat->name) != 0;

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    if (player->on_load_metadata) {
        player->on_load_metadata(player);
    }

    /* if seeking requested, we execute it */
    if (player->start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = player->start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(nullptr, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                   is->filename, (double) timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);

    if (player->show_status)
        av_dump_format(ic, 0, is->filename, 0);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && player->wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, player->wanted_stream_spec[type]) > 0)
                st_index[type] = i;
    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (player->wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(nullptr, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n",
                   player->wanted_stream_spec[i], av_get_media_type_string(static_cast<AVMediaType>(i)));
            st_index[i] = INT_MAX;
        }
    }

    if (!player->video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
                av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                    st_index[AVMEDIA_TYPE_VIDEO], -1, nullptr, 0);
    if (!player->audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
                av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                    st_index[AVMEDIA_TYPE_AUDIO],
                                    st_index[AVMEDIA_TYPE_VIDEO],
                                    nullptr, 0);
    if (!player->video_disable && !player->subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
                av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                    st_index[AVMEDIA_TYPE_SUBTITLE],
                                    (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO]
                                                                       : st_index[AVMEDIA_TYPE_VIDEO]),
                                    nullptr, 0);

    is->show_mode = player->show_mode;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        // AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        // AVCodecParameters *codecpar = st->codecpar;
        // AVRational sar = av_guess_sample_aspect_ratio(ic, st, nullptr);
        // if (codecpar->width)
        //     set_default_window_size(codecpar->width, codecpar->height, sar);
    }

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
//        stream_component_open(player, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
//        ret = stream_component_open(player, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
//        stream_component_open(player, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    if (player->infinite_buffer < 0 && is->realtime)
        player->infinite_buffer = 1;

    for (;;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
            (!strcmp(ic->iformat->name, "rtsp") ||
             (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
            int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
            // FIXME the +-2 is due to rounding being not done in the correct direction in generation
            //      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(nullptr, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->url);
            } else {
                if (is->audio_stream >= 0) {
                    is->audioq.Flush();
                    is->audioq.Put(flush_pkt);
                }
                if (is->subtitle_stream >= 0) {
                    is->subtitleq.Flush();
                    is->subtitleq.Put(flush_pkt);
                }
                if (is->video_stream >= 0) {
                    is->videoq.Flush();
                    is->videoq.Put(flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    is->extclk.SetClock(NAN, 0);
                } else {
                    is->extclk.SetClock(seek_target / (double) AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            // step to next frame
            if (ffplayer_is_paused(player)) {
                ffplayer_toggle_pause(player);
            }
//            change_player_state(player, FFP_STATE_READY);
        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy;
                if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                is->videoq.Put(&copy);
                is->videoq.PutNullPacket(is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (player->infinite_buffer < 1 &&
            (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE ||
             (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
              stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
              stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if (!is->paused &&
            (!is->audio_st ||
             (is->auddec.finished == is->audioq.serial && is->sampq.NbRemaining() == 0)) &&
            (!is->video_st ||
             (is->viddec.finished == is->videoq.serial && is->pictq.NbRemaining() == 0))) {
            bool loop = player->loop != 1 && (!player->loop || --player->loop);
            ffp_send_msg1(player, FFP_MSG_COMPLETED, loop);
            if (loop) {
//                stream_seek(player, player->start_time != AV_NOPTS_VALUE ? player->start_time : 0, 0, 0);
            } else {
                // TODO: 0 it's a bit early to notify complete here
//                change_player_state(player, FFP_STATE_END);
//                stream_toggle_pause(player->is);
            }
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    is->videoq.PutNullPacket(is->video_stream);
                if (is->audio_stream >= 0)
                    is->audioq.PutNullPacket(is->audio_stream);
                if (is->subtitle_stream >= 0)
                    is->subtitleq.PutNullPacket(is->subtitle_stream);
                is->eof = 1;
//                check_buffering(player);
            }
            if (ic->pb && ic->pb->error)
                break;
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            is->eof = 0;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = player->duration == AV_NOPTS_VALUE ||
                            (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                            av_q2d(ic->streams[pkt->stream_index]->time_base) -
                            (double) (player->start_time != AV_NOPTS_VALUE ? player->start_time : 0) / 1000000 <=
                            ((double) player->duration / 1000000);

        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            is->audioq.Put(pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range &&
                   !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            is->videoq.Put(pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            is->subtitleq.Put(pkt);
        } else {
            av_packet_unref(pkt);
        }
//        check_buffering(player);
    }

    ret = 0;
    fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    if (ret != 0) {
        //TODO close stream?
    }
    SDL_DestroyMutex(wait_mutex);
    return 0;
}

DataSource::DataSource(const char *filename, AVInputFormat *format) : in_format(format) {
    memset(wanted_stream_spec, 0, sizeof wanted_stream_spec);
    this->filename = av_strdup(filename);
}

int DataSource::Open(CPlayer *player) {
    if (!filename) {
        return -1;
    }
    continue_read_thread_ = new std::condition_variable_any();
    read_tid = new std::thread(&DataSource::ReadThread, this);
    if (!read_tid) {
        av_log(nullptr, AV_LOG_FATAL, "can not create thread for video render.\n");
        return -1;
    }
    return 0;
    fail:
    return -1;
}

void DataSource::Close() {

}

DataSource::~DataSource() {
    av_free(filename);
}

void DataSource::ReadThread() {
    av_log(nullptr, AV_LOG_DEBUG, "DataSource Read Start: %s \n", filename);
    int st_index[AVMEDIA_TYPE_NB] = {-1, -1, -1, -1, -1};
    std::mutex wait_mutex;

    if (PrepareFormatContext() < 0) {
        return;
    }
    OnFormatContextOpen();

    ReadStreamInfo(st_index);
    OnStreamInfoLoad(st_index);

    if (OpenStreams(st_index) < 0) {
        // todo destroy streams;
        return;
    }
    ReadStreams(&wait_mutex);
}

int DataSource::PrepareFormatContext() {
    format_ctx_ = avformat_alloc_context();
    if (!format_ctx_) {
        av_log(nullptr, AV_LOG_FATAL, "Could not allocate context.\n");
        return -1;
    }
    format_ctx_->interrupt_callback.opaque = this;
    format_ctx_->interrupt_callback.callback = [](void *ctx) -> int {
        auto *source = static_cast<DataSource *>(ctx);
        return source->abort_request;
    };
    auto err = avformat_open_input(&format_ctx_, filename, in_format, nullptr);
    if (err < 0) {
        av_log(nullptr, AV_LOG_ERROR, "can not open file %s: %s", filename, av_err2str(err));
        avformat_free_context(format_ctx_);
        return -1;
    }

    if (gen_pts) {
        format_ctx_->flags |= AVFMT_FLAG_GENPTS;
    }

    av_format_inject_global_side_data(format_ctx_);

    if (format_ctx_->pb) {
        format_ctx_->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
    }

    if (seek_by_bytes < 0) {
        seek_by_bytes = (format_ctx_->iformat->flags & AVFMT_TS_DISCONT) != 0
                        && strcmp("ogg", format_ctx_->iformat->name) != 0;
    }

    max_frame_duration = (format_ctx_->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    return 0;
}


void DataSource::OnFormatContextOpen() {
    if (on_load_metadata != nullptr) {
        on_load_metadata();
    }

    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {
        auto timestamp = start_time;
        if (format_ctx_->start_time != AV_NOPTS_VALUE) {
            timestamp += format_ctx_->start_time;
        }
        auto ret = avformat_seek_file(format_ctx_, -1, INT16_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(nullptr, AV_LOG_WARNING, "%s: could not seek to position %0.3f. err = %s\n",
                   filename, (double) timestamp / AV_TIME_BASE, av_err2str(ret));
        }
    }

    realtime_ = is_realtime(format_ctx_);
    if (!infinite_buffer && realtime_) {
        infinite_buffer = true;
    }

    if (show_status) {
        av_dump_format(format_ctx_, 0, filename, 0);
    }
}

int DataSource::ReadStreamInfo(int st_index[AVMEDIA_TYPE_NB]) {
    for (int i = 0; i < format_ctx_->nb_streams; ++i) {
        auto *st = format_ctx_->streams[i];
        auto type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type > 0 && wanted_stream_spec[type] && st_index[type] == -1) {
            if (avformat_match_stream_specifier(format_ctx_, st, wanted_stream_spec[type]) > 0) {
                st_index[type] = i;
            }
        }
    }

    for (int i = 0; i < AVMEDIA_TYPE_NB; ++i) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(nullptr, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n",
                   wanted_stream_spec[i], av_get_media_type_string(static_cast<AVMediaType>(i)));
            st_index[i] = INT_MAX;
        }
    }

    if (!video_disable) {
        st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO,
                                                           st_index[AVMEDIA_TYPE_VIDEO], -1,
                                                           nullptr, 0);
    }
    if (!audio_disable) {
        st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_AUDIO,
                                                           st_index[AVMEDIA_TYPE_AUDIO],
                                                           st_index[AVMEDIA_TYPE_VIDEO],
                                                           nullptr, 0);
    }
    if (!video_disable && !subtitle_disable) {
        st_index[AVMEDIA_TYPE_SUBTITLE] = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_SUBTITLE,
                                                              st_index[AVMEDIA_TYPE_SUBTITLE],
                                                              (st_index[AVMEDIA_TYPE_AUDIO] >= 0
                                                               ? st_index[AVMEDIA_TYPE_AUDIO]
                                                               : st_index[AVMEDIA_TYPE_VIDEO]),
                                                              nullptr, 0);
    }


    return 0;
}

void DataSource::OnStreamInfoLoad(const int st_index[AVMEDIA_TYPE_NB]) {
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        auto *st = format_ctx_->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        auto sar = av_guess_sample_aspect_ratio(format_ctx_, st, nullptr);
        if (st->codecpar->width) {
            // TODO: frame size available...
        }
    }
}

int DataSource::OpenStreams(const int st_index[AVMEDIA_TYPE_NB]) {
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        OpenComponentStream(st_index[AVMEDIA_TYPE_AUDIO], AVMEDIA_TYPE_AUDIO);
    }
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        OpenComponentStream(st_index[AVMEDIA_TYPE_VIDEO], AVMEDIA_TYPE_VIDEO);
    }
    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        OpenComponentStream(st_index[AVMEDIA_TYPE_SUBTITLE], AVMEDIA_TYPE_SUBTITLE);
    }
    if (video_stream_index < 0 && audio_stream_index < 0) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n", filename);
        return -1;
    }
    return 0;
}

int DataSource::OpenComponentStream(int stream_index, AVMediaType media_type) {
    if (decoder_ctx == nullptr) {
        av_log(nullptr, AV_LOG_ERROR, "can not open stream(%d) cause decoder_ctx is null.\n", stream_index);
        return -1;
    }
    if (stream_index < 0 || stream_index >= format_ctx_->nb_streams) {
        return -1;
    }
    auto stream = format_ctx_->streams[stream_index];

    DecodeParams params;
    switch (media_type) {
        case AVMEDIA_TYPE_VIDEO:
            params.pkt_queue = video_queue;
            break;
        case AVMEDIA_TYPE_AUDIO:
            params.pkt_queue = audio_queue;
            params.audio_follow_stream_start_pts =
                    (format_ctx_->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK))
                    && format_ctx_->iformat->read_seek;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            params.pkt_queue = subtitle_queue;
            break;
        default:
            return -1;
    }
    params.read_condition = this->continue_read_thread_;
    params.stream = stream;
    params.format_ctx = format_ctx_;

    if (decoder_ctx->StartDecoder(&params) >= 0) {
        switch (media_type) {
            case AVMEDIA_TYPE_VIDEO:
                video_stream_index = stream_index;
                video_stream_ = stream;
                break;
            case AVMEDIA_TYPE_AUDIO:
                audio_stream_index = stream_index;
                audio_stream_ = stream;
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                subtitle_stream_index = stream_index;
                subtitle_stream_ = stream;
                break;
            default:
                break;
        }
    }
    return 0;
}

void DataSource::ReadStreams(std::mutex *read_mutex) {
    bool last_paused = false;
    AVPacket pkt_data, *pkt = &pkt_data;
    for (;;) {
        if (abort_request) {
            break;
        }
        if (paused != last_paused) {
            last_paused = paused;
            if (paused) {
                av_read_pause(format_ctx_);
            } else {
                av_read_play(format_ctx_);
            }
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (paused && (!strcmp(format_ctx_->iformat->name, "rtsp")
                       || (format_ctx_->pb && !strncmp(filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        ProcessSeekRequest();
        ProcessAttachedPicture();
        if (!isNeedReadMore()) {
            std::unique_lock<std::mutex> lock(*read_mutex);
            continue_read_thread_->wait(lock);
            continue;
        }
        if (IsReadComplete()) {
            // TODO check complete
//            bool loop = player->loop != 1 && (!player->loop || --player->loop);
//            ffp_send_msg1(player, FFP_MSG_COMPLETED, loop);
//            if (loop) {
//                stream_seek(player, player->start_time != AV_NOPTS_VALUE ? player->start_time : 0, 0, 0);
//            } else {
            // TODO: 0 it's a bit early to notify complete here
//                change_player_state(player, FFP_STATE_END);
//                stream_toggle_pause(player->is);
//            }
        }
        {
            auto ret = ProcessReadFrame(pkt, read_mutex);
            if (ret < 0) {
                break;
            } else if (ret > 0) {
                continue;
            }
        }

        ProcessQueuePacket(pkt);
//        check_buffering(player);
    }
}

void DataSource::ProcessSeekRequest() {
    if (!seek_req_) {
        return;
    }
    auto seek_target = seek_position;
    auto ret = avformat_seek_file(format_ctx_, -1, INT64_MIN, seek_target, INT64_MAX, 0);
    if (ret < 0) {
        av_log(nullptr, AV_LOG_ERROR, "%s: error while seeking, error: %s\n", filename, av_err2str(ret));
    } else {
        if (audio_stream_index >= 0 && audio_queue) {
            audio_queue->Flush();
            audio_queue->Put(flush_pkt);
        }
        if (subtitle_stream_index >= 0 && subtitle_queue) {
            subtitle_queue->Flush();
            subtitle_queue->Put(flush_pkt);
        }
        if (video_stream_index >= 0 && video_queue) {
            video_queue->Flush();
            video_queue->Put(flush_pkt);
        }
        if (ext_clock) {
            ext_clock->SetClock(seek_target / (double) AV_TIME_BASE, 0);
        }
    }
    seek_req_ = false;
    queue_attachments_req_ = true;
    eof = false;

    // TODO notify on seek complete.
}

void DataSource::ProcessAttachedPicture() {
    if (!queue_attachments_req_) {
        return;
    }
    if (video_stream_ && video_stream_->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        AVPacket copy;
        auto ret = av_packet_ref(&copy, &video_stream_->attached_pic);
        if (ret < 0) {
            av_log(nullptr, AV_LOG_ERROR, "%s: error to read attached pic. error: %s", filename, av_err2str(ret));
        } else {
            video_queue->Put(&copy);
            video_queue->PutNullPacket(video_stream_index);
        }
    }
    queue_attachments_req_ = false;
}

bool DataSource::isNeedReadMore() {
    if (infinite_buffer) {
        return true;
    }
    if (audio_queue->size + video_queue->size + subtitle_queue->size > MAX_QUEUE_SIZE) {
        return false;
    }
    if (stream_has_enough_packets(audio_stream_, audio_stream_index, audio_queue)
        && stream_has_enough_packets(video_stream_, video_stream_index, video_queue)
        && stream_has_enough_packets(subtitle_stream_, subtitle_stream_index, subtitle_queue)) {
        return false;
    }
    return true;
}

bool DataSource::IsReadComplete() const {
    if (paused) {
        return false;
    }
    return false;
}

int DataSource::ProcessReadFrame(AVPacket *pkt, std::mutex *read_mutex) {
    auto ret = av_read_frame(format_ctx_, pkt);
    if (ret < 0) {
        if ((ret == AVERROR_EOF || avio_feof(format_ctx_->pb)) && !eof) {
            if (video_stream_index >= 0) {
                video_queue->PutNullPacket(video_stream_index);
            }
            if (audio_stream_index >= 0) {
                video_queue->PutNullPacket(audio_stream_index);
            }
            if (subtitle_stream_index >= 0) {
                video_queue->PutNullPacket(subtitle_stream_index);
            }
            eof = true;
            //                check_buffering(player);
        }
        if (format_ctx_->pb && format_ctx_->pb->error) {
            return -1;
        }
        std::unique_lock<std::mutex> lock(*read_mutex);
        continue_read_thread_->wait_for(lock, std::chrono::milliseconds(10));
        return 1;
    } else {
        eof = false;
    }
    return 0;
}

void DataSource::ProcessQueuePacket(AVPacket *pkt) {
    auto stream_start_time = format_ctx_->streams[pkt->stream_index]->start_time;
    if (stream_start_time == AV_NOPTS_VALUE) {
        stream_start_time = 0;
    }
    auto pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    auto player_start_time = start_time == AV_NOPTS_VALUE ? start_time : 0;
    auto diff = (double) (pkt_ts - stream_start_time) * av_q2d(format_ctx_->streams[pkt->stream_index]->time_base)
                - player_start_time / (double) AV_TIME_BASE;
    bool pkt_in_play_range = duration == AV_NOPTS_VALUE
                             || diff <= duration / (double) AV_TIME_BASE;
    if (pkt->stream_index == audio_stream_index && pkt_in_play_range) {
        audio_queue->Put(pkt);
    } else if (pkt->stream_index == video_stream_index && pkt_in_play_range &&
               !(video_stream_->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        video_queue->Put(pkt);
    } else if (pkt->stream_index == subtitle_stream_index && pkt_in_play_range) {
        subtitle_queue->Put(pkt);
    } else {
        av_packet_unref(pkt);
    }
}

bool DataSource::ContainVideoStream() {
    return video_stream_ != nullptr;
}

bool DataSource::ContainAudioStream() {
    return audio_stream_ != nullptr;
}

