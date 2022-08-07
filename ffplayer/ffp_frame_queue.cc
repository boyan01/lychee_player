//
// Created by boyan on 2021/2/10.
//

#include "ffp_frame_queue.h"

void Frame::Unref() {
  av_frame_unref(this->frame);
  avsubtitle_free(&this->sub);
}

int FrameQueue::Init(PacketQueue* _pktq, int _max_size, int _keep_last) {
  this->pktq = _pktq;
  this->max_size = FFMIN(_max_size, FRAME_QUEUE_SIZE);
  this->keep_last = !!_keep_last;
  for (int i = 0; i < this->max_size; i++)
    if (!(this->queue[i].frame = av_frame_alloc()))
      return AVERROR(ENOMEM);
  return 0;
}

void FrameQueue::Destroy() {
  auto f = this;
  int i;
  for (i = 0; i < f->max_size; i++) {
    Frame* vp = &f->queue[i];
    vp->Unref();
    av_frame_free(&vp->frame);
  }
}

void FrameQueue::Signal() {
  cond.notify_all();
}

Frame* FrameQueue::Peek() {
  auto f = this;
  return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

Frame* FrameQueue::PeekNext() {
  auto f = this;
  return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

Frame* FrameQueue::PeekLast() {
  auto f = this;
  return &f->queue[f->rindex];
}

Frame* FrameQueue::PeekWritable() {
  auto f = this;
  /* wait until we have space to put a new frame */
  std::unique_lock<std::recursive_mutex> lock(mutex);
  while (f->size >= f->max_size && !f->pktq->abort_request) {
    cond.wait(lock);
  }

  if (f->pktq->abort_request)
    return nullptr;

  return &f->queue[f->windex];
}

Frame* FrameQueue::PeekReadable() {
  auto f = this;
  /* wait until we have a readable a new frame */
  std::unique_lock<std::recursive_mutex> lock(mutex);
  while (f->size - f->rindex_shown <= 0 && !f->pktq->abort_request) {
    cond.wait(lock);
  }

  if (f->pktq->abort_request)
    return nullptr;

  return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

void FrameQueue::Push() {
  auto f = this;
  if (++f->windex == f->max_size)
    f->windex = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    f->size++;
  }
  cond.notify_all();
}

void FrameQueue::Next() {
  auto f = this;
  if (f->keep_last && !f->rindex_shown) {
    f->rindex_shown = 1;
    return;
  }
  f->queue[f->rindex].Unref();
  if (++f->rindex == f->max_size)
    f->rindex = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    f->size--;
  }
  cond.notify_all();
}

int FrameQueue::NbRemaining() {
  auto f = this;
  return f->size - f->rindex_shown;
}

int64_t FrameQueue::LastPos() {
  auto f = this;
  Frame* fp = &f->queue[f->rindex];
  if (f->rindex_shown && fp->serial == f->pktq->serial)
    return fp->pos;
  else
    return -1;
}
