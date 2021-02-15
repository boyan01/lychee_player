//
// Created by boyan on 2021/1/23.
//

#ifndef FFPLAYER_FFP_MSG_QUEUE_H
#define FFPLAYER_FFP_MSG_QUEUE_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

extern "C" {
#include "libavutil/log.h"
};

#define FFP_MSG_FLUSH                       0
#define FFP_MSG_ERROR                       100     /* arg1 = error */
#define FFP_MSG_PREPARED                    200
#define FFP_MSG_COMPLETED                   300
#define FFP_MSG_VIDEO_SIZE_CHANGED          400     /* arg1 = width, arg2 = height */
#define FFP_MSG_SAR_CHANGED                 401     /* arg1 = sar.num, arg2 = sar.den */
#define FFP_MSG_VIDEO_RENDERING_START       402
#define FFP_MSG_AUDIO_RENDERING_START       403
#define FFP_MSG_VIDEO_ROTATION_CHANGED      404     /* arg1 = degree */
#define FFP_MSG_AUDIO_DECODED_START         405
#define FFP_MSG_VIDEO_DECODED_START         406
#define FFP_MSG_OPEN_INPUT                  407
#define FFP_MSG_FIND_STREAM_INFO            408
#define FFP_MSG_COMPONENT_OPEN              409
#define FFP_MSG_VIDEO_SEEK_RENDERING_START  410
#define FFP_MSG_AUDIO_SEEK_RENDERING_START  411
#define FFP_MSG_VIDEO_FRAME_LOADED          412     /* arg1 = display width, arg2 = display height */
#define FFP_MSG_AV_METADATA_LOADED          413

#define FFP_MSG_BUFFERING_START             500
#define FFP_MSG_BUFFERING_END               501
#define FFP_MSG_BUFFERING_UPDATE            502     /* arg1 = buffering head position in time, arg2 = minimum percent in time or bytes */
#define FFP_MSG_BUFFERING_BYTES_UPDATE      503     /* arg1 = cached data in bytes,            arg2 = high water mark */
#define FFP_MSG_BUFFERING_TIME_UPDATE       504     /* arg1 = cached duration in milliseconds, arg2 = high water mark */
#define FFP_MSG_SEEK_COMPLETE               600     /* arg1 = seek position,                   arg2 = error */
#define FFP_MSG_PLAYBACK_STATE_CHANGED      700
#define FFP_MSG_TIMED_TEXT                  800
#define FFP_MSG_ACCURATE_SEEK_COMPLETE      900     /* arg1 = current position*/
#define FFP_MSG_GET_IMG_STATE               1000    /* arg1 = timestamp, arg2 = result code, obj = file name*/

#define FFP_MSG_VIDEO_DECODER_OPEN          10001

#define FFP_REQ_START                       20001
#define FFP_REQ_PAUSE                       20002
#define FFP_REQ_SEEK                        20003

#define FFP_PROP_FLOAT_VIDEO_DECODE_FRAMES_PER_SECOND   10001
#define FFP_PROP_FLOAT_VIDEO_OUTPUT_FRAMES_PER_SECOND   10002
#define FFP_PROP_FLOAT_PLAYBACK_RATE                    10003
#define FFP_PROP_FLOAT_PLAYBACK_VOLUME                  10006
#define FFP_PROP_FLOAT_AVDELAY                          10004
#define FFP_PROP_FLOAT_AVDIFF                           10005
#define FFP_PROP_FLOAT_DROP_FRAME_RATE                  10007

#define FFP_PROP_INT64_SELECTED_VIDEO_STREAM            20001
#define FFP_PROP_INT64_SELECTED_AUDIO_STREAM            20002
#define FFP_PROP_INT64_SELECTED_TIMEDTEXT_STREAM        20011
#define FFP_PROP_INT64_VIDEO_DECODER                    20003
#define FFP_PROP_INT64_AUDIO_DECODER                    20004
#define     FFP_PROPV_DECODER_UNKNOWN                   0
#define     FFP_PROPV_DECODER_AVCODEC                   1
#define     FFP_PROPV_DECODER_MEDIACODEC                2
#define     FFP_PROPV_DECODER_VIDEOTOOLBOX              3
#define FFP_PROP_INT64_VIDEO_CACHED_DURATION            20005
#define FFP_PROP_INT64_AUDIO_CACHED_DURATION            20006
#define FFP_PROP_INT64_VIDEO_CACHED_BYTES               20007
#define FFP_PROP_INT64_AUDIO_CACHED_BYTES               20008
#define FFP_PROP_INT64_VIDEO_CACHED_PACKETS             20009
#define FFP_PROP_INT64_AUDIO_CACHED_PACKETS             20010

#define FFP_PROP_INT64_BIT_RATE                         20100

#define FFP_PROP_INT64_TCP_SPEED                        20200

#define FFP_PROP_INT64_ASYNC_STATISTIC_BUF_BACKWARDS    20201
#define FFP_PROP_INT64_ASYNC_STATISTIC_BUF_FORWARDS     20202
#define FFP_PROP_INT64_ASYNC_STATISTIC_BUF_CAPACITY     20203
#define FFP_PROP_INT64_TRAFFIC_STATISTIC_BYTE_COUNT     20204

#define FFP_PROP_INT64_LATEST_SEEK_LOAD_DURATION        20300

#define FFP_PROP_INT64_CACHE_STATISTIC_PHYSICAL_POS     20205

#define FFP_PROP_INT64_CACHE_STATISTIC_FILE_FORWARDS    20206

#define FFP_PROP_INT64_CACHE_STATISTIC_FILE_POS         20207

#define FFP_PROP_INT64_CACHE_STATISTIC_COUNT_BYTES      20208

#define FFP_PROP_INT64_LOGICAL_FILE_SIZE                20209
#define FFP_PROP_INT64_SHARE_CACHE_DATA                 20210
#define FFP_PROP_INT64_IMMEDIATE_RECONNECT              20211

struct MessageQueue;

class MessageContext {

 public:

  std::function<void(int what, int64_t arg1, int64_t arg2)> message_callback{nullptr};

 private:
  std::unique_ptr<MessageQueue> msg_queue{};
  bool started_ = false;
  std::thread *thread_ = nullptr;

 private:

  void MessageThread();

  void StopAndWait();

  void Start();

 public:
  MessageContext();

  ~MessageContext();

  void NotifyMsg(int what, int arg1, int arg2);

  void NotifyMsg(int what, int64_t arg1);

  void NotifyMsg(int what);

};

#endif //FFPLAYER_FFP_MSG_QUEUE_H
