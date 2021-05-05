//
// Created by boyan on 2021/2/10.
//

#ifndef FFPLAYER_FFP_FRAME_QUEUE_H
#define FFPLAYER_FFP_FRAME_QUEUE_H

#include <condition_variable>
#include <mutex>

#include "ffp_packet_queue.h"

extern "C" {
#include "libavcodec/avcodec.h"
};

namespace media {

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

/* Common struct for handling all types of decoded data and allocated render buffers. */
struct Frame {
  AVFrame *frame;
  AVSubtitle sub;
  int serial;
  double pts;      /* presentation timestamp for the frame */
  double duration; /* estimated duration of the frame */
  int64_t pos;     /* byte position of the frame in the input file */
  int width;
  int height;
  int format;
  AVRational sar;
  int uploaded;
  int flip_v;

 public:
  void Unref();
};

class FrameQueue {
 public:
  Frame queue[FRAME_QUEUE_SIZE];
  int rindex = 0;
  int windex = 0;
  int size = 0;
  int max_size = 0;
  bool keep_last = false;
  int rindex_shown = 0;
  std::recursive_mutex mutex;
  std::condition_variable_any cond;
  PacketQueue *pktq = nullptr;

 public:
  int Init(PacketQueue *_pktq, int _max_size, bool _keep_last);

  void Destroy();

  void Signal();

  Frame *Peek();

  Frame *PeekNext();

  Frame *PeekLast();

  Frame *PeekWritable();

  Frame *PeekReadable();

  void Push();

  void Next();

  /* return the number of undisplayed frames in the queue */
  int NbRemaining();

  /* return last shown position */
  int64_t LastPos();

};

}

#endif //FFPLAYER_FFP_FRAME_QUEUE_H
