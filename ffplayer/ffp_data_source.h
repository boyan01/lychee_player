//
// Created by boyan on 2021/2/10.
//

#ifndef FFPLAYER_FFP_DATA_SOURCE_H
#define FFPLAYER_FFP_DATA_SOURCE_H

#include <thread>
#include <condition_variable>
#include <functional>

#include "ffp_packet_queue.h"
#include "ffp_clock.h"
#include "ffp_decoder.h"

extern "C" {
#include "libavformat/avformat.h"
#include "SDL2/SDL.h"
};

class DataSource {
public:
    bool gen_pts = false;
    bool find_stream_info = true;
    int seek_by_bytes = -1;

    std::function<void()> on_load_metadata = nullptr;

    int64_t start_time = AV_NOPTS_VALUE;

    bool show_status = false;
    char *wanted_stream_spec[AVMEDIA_TYPE_NB];

    bool video_disable = false;
    bool audio_disable = false;
    bool subtitle_disable = false;
    bool infinite_buffer = false;

    PacketQueue *audio_queue;
    PacketQueue *video_queue;
    PacketQueue *subtitle_queue;

    Clock *ext_clock;

    DecoderContext *decoder_ctx;

    int read_pause_return;

    bool paused = false;

    void Seek(double position);

    double GetDuration();

    int GetChapterCount();

    int GetChapterByPosition(int64_t position);

    void SeekToChapter(int chapter);

    const char *GetMetadataDict(const char *key);

private:
    char *filename;
    AVInputFormat *in_format;
    std::condition_variable_any *continue_read_thread_ = nullptr;
    std::thread *read_tid = nullptr;
    bool abort_request = false;
    AVFormatContext *format_ctx_ = nullptr;
    bool realtime_ = false;

    int audio_stream_index = -1;
    int video_stream_index = -1;
    int subtitle_stream_index = -1;

    // request for seek.
    bool seek_req_ = false;
    int64_t seek_position = 0;

    // request for attached_pic.
    bool queue_attachments_req_ = false;

    bool eof = false;

    AVStream *video_stream_ = nullptr;
    AVStream *audio_stream_ = nullptr;
    AVStream *subtitle_stream_ = nullptr;

    int64_t duration = AV_NOPTS_VALUE;

public:

    DataSource(const char *filename, AVInputFormat *format);

    ~DataSource();

    int Open(CPlayer *player);

    void Close();

    bool ContainVideoStream();

    bool ContainAudioStream();

    bool ContainSubtitleStream();

    double GetSeekPosition() const;

    const char *GetFileName() const;

private:

    int PrepareFormatContext();

    void OnFormatContextOpen();

    int ReadStreamInfo(int st_index[AVMEDIA_TYPE_NB]);

    void ReadThread();

    void OnStreamInfoLoad(const int st_index[AVMEDIA_TYPE_NB]);

    int OpenStreams(const int st_index[AVMEDIA_TYPE_NB]);

    int OpenComponentStream(int stream_index, AVMediaType media_type);

    void ReadStreams(std::mutex *read_mutex);

    void ProcessSeekRequest();

    void ProcessAttachedPicture();

    bool isNeedReadMore();

    bool IsReadComplete() const;

    int ProcessReadFrame(AVPacket *pkt, std::mutex *read_mutex);

    void ProcessQueuePacket(AVPacket *pkt);

};

#endif //FFPLAYER_FFP_DATA_SOURCE_H
