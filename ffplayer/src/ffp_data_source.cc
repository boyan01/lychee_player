//
// Created by boyan on 2021/2/10.
//

#include "ffplayer.h"
#include "ffp_data_source.h"
#include "ffp_utils.h"

#define MIN_FRAMES 25
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)

char av_error[AV_ERROR_MAX_STRING_SIZE] = {0};
#define av_err2str(errnum) av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)

extern AVPacket *flush_pkt;

static const AVRational av_time_base_q_ = {1, AV_TIME_BASE};

static inline int stream_has_enough_packets(AVStream *st, int stream_id, const std::shared_ptr<PacketQueue> &queue) {
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

DataSource::DataSource(const char *filename, AVInputFormat *format) : in_format(format) {
  memset(wanted_stream_spec, 0, sizeof wanted_stream_spec);
  this->filename = av_strdup(filename);
  continue_read_thread_ = std::make_shared<std::condition_variable_any>();
}

int DataSource::Open() {
  if (!filename) {
    return -1;
  }
  read_tid = new std::thread(&DataSource::ReadThread, this);
  if (!read_tid) {
    av_log(nullptr, AV_LOG_FATAL, "can not create thread for video render.\n");
    return -1;
  }
  return 0;
}

DataSource::~DataSource() {
  av_free(filename);
  if (read_tid && read_tid->joinable()) {
    abort_request = true;
    continue_read_thread_->notify_all();
    read_tid->join();
  }
  if (format_ctx_) {
    avformat_free_context(format_ctx_);
    format_ctx_ = nullptr;
  }
}

void DataSource::ReadThread() {
  update_thread_name("read_source");
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

  av_log(nullptr, AV_LOG_INFO, "thread: read_source done.\n");
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
    return -1;
  }

  if (gen_pts) {
    format_ctx_->flags |= AVFMT_FLAG_GENPTS;
  }

  av_format_inject_global_side_data(format_ctx_);

  // find stream info for av file. this is useful for formats with no headers such as MPEG.
  if (find_stream_info && avformat_find_stream_info(format_ctx_, nullptr) < 0) {
    avformat_free_context(format_ctx_);
    format_ctx_ = nullptr;
    return -1;
  }

  if (format_ctx_->pb) {
    format_ctx_->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
  }

  if (seek_by_bytes < 0) {
    seek_by_bytes = (format_ctx_->iformat->flags & AVFMT_TS_DISCONT) != 0
        && strcmp("ogg", format_ctx_->iformat->name) != 0;
  }

//    max_frame_duration = (format_ctx_->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

  return 0;
}

void DataSource::OnFormatContextOpen() {
  msg_ctx->NotifyMsg(FFP_MSG_AV_METADATA_LOADED);

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

  if (configuration.show_status) {
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

  if (!configuration.video_disable) {
    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO,
                                                       st_index[AVMEDIA_TYPE_VIDEO], -1,
                                                       nullptr, 0);
  }
  if (!configuration.audio_disable) {
    st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_AUDIO,
                                                       st_index[AVMEDIA_TYPE_AUDIO],
                                                       st_index[AVMEDIA_TYPE_VIDEO],
                                                       nullptr, 0);
  }
  if (!configuration.video_disable && !configuration.subtitle_disable) {
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

  std::unique_ptr<DecodeParams> params;
  switch (media_type) {
    case AVMEDIA_TYPE_VIDEO:
      params = std::make_unique<DecodeParams>(video_queue,
                                              continue_read_thread_,
                                              &format_ctx_,
                                              stream_index);
      break;
    case AVMEDIA_TYPE_AUDIO:
      params = std::make_unique<DecodeParams>(audio_queue,
                                              continue_read_thread_,
                                              &format_ctx_,
                                              stream_index);
      params->audio_follow_stream_start_pts =
          (format_ctx_->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK))
              && format_ctx_->iformat->read_seek;
      break;
    case AVMEDIA_TYPE_SUBTITLE:
      params = std::make_unique<DecodeParams>(subtitle_queue,
                                              continue_read_thread_,
                                              &format_ctx_,
                                              stream_index);
      break;
    default:return -1;
  }

  if (decoder_ctx->StartDecoder(std::move(params)) >= 0) {
    switch (media_type) {
      case AVMEDIA_TYPE_VIDEO:video_stream_index = stream_index;
        video_stream_ = stream;
        break;
      case AVMEDIA_TYPE_AUDIO:audio_stream_index = stream_index;
        audio_stream_ = stream;
        break;
      case AVMEDIA_TYPE_SUBTITLE:subtitle_stream_index = stream_index;
        subtitle_stream_ = stream;
        break;
      default:break;
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

bool DataSource::ContainSubtitleStream() {
  return subtitle_stream_ != nullptr;
}

void DataSource::Seek(double position) {
  int64_t target = position * AV_TIME_BASE;
  if (!format_ctx_) {
    start_time = FFMAX(0, target);
    return;
  }

  if (format_ctx_->start_time != AV_NOPTS_VALUE) {
    position = FFMAX(format_ctx_->start_time, target);
  }
  target = FFMAX(0, target);
  target = FFMIN(target, format_ctx_->duration);
  av_log(nullptr, AV_LOG_INFO, "data source seek to %0.2f \n", position);

  if (!seek_req_) {
    seek_position = target;
    seek_req_ = true;

    // TODO update buffered position
//        player->buffered_position = -1;
//        change_player_state(player, FFP_STATE_BUFFERING);
    continue_read_thread_->notify_all();
  }
}

double DataSource::GetSeekPosition() const { return seek_position / (double) (AV_TIME_BASE); }

double DataSource::GetDuration() {
  CHECK_VALUE_WITH_RETURN(format_ctx_, -1);
  return format_ctx_->duration / (double) AV_TIME_BASE;
}

int DataSource::GetChapterCount() {
  CHECK_VALUE_WITH_RETURN(format_ctx_, -1);
  return (int) format_ctx_->nb_chapters;
}

int DataSource::GetChapterByPosition(int64_t position) {
  CHECK_VALUE_WITH_RETURN(format_ctx_, -1);
  CHECK_VALUE_WITH_RETURN(format_ctx_->nb_chapters, -1);
  for (int i = 0; i < format_ctx_->nb_chapters; i++) {
    AVChapter *ch = format_ctx_->chapters[i];
    if (av_compare_ts(position, av_time_base_q_, ch->start, ch->time_base) < 0) {
      i--;
      return i;
    }
  }
  return -1;
}

void DataSource::SeekToChapter(int chapter) {
  CHECK_VALUE(format_ctx_);
  CHECK_VALUE(format_ctx_->nb_chapters);
  if (chapter < 0 || chapter >= format_ctx_->nb_chapters) {
    av_log(nullptr, AV_LOG_ERROR, "chapter out of range: %d", chapter);
    return;
  }
  AVChapter *ac = format_ctx_->chapters[chapter];
  Seek(av_rescale_q(ac->start, ac->time_base, av_time_base_q_));
}

const char *DataSource::GetFileName() const { return filename; }

const char *DataSource::GetMetadataDict(const char *key) {
  CHECK_VALUE_WITH_RETURN(format_ctx_, nullptr);
  auto *entry = av_dict_get(format_ctx_->metadata, key, nullptr, 0);
  if (entry) {
    return entry->value;
  }
  return nullptr;
}


