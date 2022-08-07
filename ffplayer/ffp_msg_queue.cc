//
// Created by boyan on 2021/2/6.
//

#include "ffp_msg_queue.h"
#include "ffp_utils.h"
#include "logging.h"

extern "C" {
#include "libavutil/common.h"
}

void MessageContext::MessageThread() {
  update_thread_name("message_loop");
  while (true) {
    auto msg = message_queue_->next();
    if (msg == nullptr) {
      DLOG(INFO) << "message_thread over.";
      return;
    }
    DCHECK(msg->next == nullptr);

    if (message_callback) {
      message_callback(msg->what, msg->arg1, msg->arg2);
    }
    delete msg;
  }
  DLOG(WARNING) << "message_thread quit.";
}

MessageContext::MessageContext()
    : message_queue_(std::make_unique<MessageQueue>()) {
  Start();
}

MessageContext::~MessageContext() {
  StopAndWait();
  DLOG(INFO) << "~MessageContext";
}

void MessageContext::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  thread_ = new std::thread(&MessageContext::MessageThread, this);
}

void MessageContext::NotifyMsg(int what, int64_t arg1, int64_t arg2) {
  if (!started_) {
    av_log(nullptr, AV_LOG_WARNING,
           "failed to notify msg to MessageContext which not started.\n");
    return;
  }
  Message msg = {0};
  msg.what = what;
  msg.arg1 = arg1;
  msg.arg2 = arg2;
  msg.next = nullptr;
  message_queue_->EnqueueMessage(msg);
}

void MessageContext::NotifyMsg(int what) {
  NotifyMsg(what, 0);
}

void MessageContext::NotifyMsg(int what, int64_t arg1) {
  NotifyMsg(what, arg1, 0);
}

void MessageContext::StopAndWait() {
  if (!started_) {
    return;
  }
  started_ = false;
  message_queue_->Quit();
  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
  delete thread_;
}
