//
// Created by boyan on 2021/2/6.
//

#include "ffp_msg_queue.h"

extern "C" {
#include "libavutil/common.h"
}


int MessageQueue::Init() {
    memset(this, 0, sizeof(MessageQueue));
    mutex_ = new std::mutex;
    cond_ = new std::condition_variable_any;
    abort_request_ = 1;
    return 0;
}

int MessageQueue::Put(Message *msg) {
    int ret;
    mutex_->lock();
    ret = PutPrivate(msg);
    mutex_->unlock();
    return ret;
}

int MessageQueue::PutPrivate(Message *msg) {
    Message *msg1;

    if (abort_request_)
        return -1;

    msg1 = (Message *) av_malloc(sizeof(Message));
    if (!msg1) {
        av_log(nullptr, AV_LOG_ERROR, "no memory to alloc msg");
        return -1;
    }

    *msg1 = *msg;
    msg1->next = nullptr;

    if (!last_) {
        first_ = msg1;
    } else {
        last_->next = msg1;
    }
    nb_messages_++;
    cond_->notify_all();
    return 0;
}

void MessageQueue::Flush() {
    Message *msg, *msg1;

    mutex_->lock();
    for (msg = first_; msg; msg = msg1) {
        msg1 = msg->next;
        av_freep(&msg);
    }
    last_ = nullptr;
    first_ = nullptr;
    nb_messages_ = 0;
    mutex_->unlock();
}

void MessageQueue::Destroy() {
    Flush();
    delete mutex_;
    delete cond_;
}

void MessageQueue::Abort() {
    mutex_->lock();

    abort_request_ = 1;

    cond_->notify_all();
    mutex_->unlock();
}

void MessageQueue::Start() {
    mutex_->lock();
    abort_request_ = 0;
    Message msg = {0};
    msg.what = FFP_MSG_FLUSH;
    PutPrivate(&msg);
    mutex_->unlock();
}

int MessageQueue::Get(Message *msg, int block) {
    Message *msg1;
    int ret;

    std::unique_lock<std::mutex> lock(*mutex_);

    for (;;) {
        if (abort_request_) {
            ret = -1;
            break;
        }

        msg1 = first_;
        if (msg1) {
            first_ = msg1->next;
            if (!first_) {
                last_ = nullptr;
            }
            nb_messages_--;
            *msg = *msg1;
            av_free(msg1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            cond_->wait(lock);
        }
    }

    return ret;
}

void MessageQueue::Remove(int what) {
    Message *p_msg, *msg, *last_msg;
    mutex_->lock();

    if (!abort_request_ && first_) {
        p_msg = first_;
        while (p_msg) {
            msg = p_msg;
            if (msg->what == what) {
                p_msg = msg->next;
                av_free(msg);
                nb_messages_--;
            } else {
                last_msg = msg;
                p_msg = msg->next;
            }
        }

        if (first_) {
            last_ = last_msg;
        } else {
            last_ = nullptr;
        }
    }

    mutex_->unlock();
}

void MessageContext::MessageThread() {
    while (true) {
        Message msg = {0};
        if (msg_queue->Get(&msg, true) < 0) {
            break;
        }
        if (message_callback) {
            message_callback(msg.what, msg.arg1, msg.arg2);
        }
    }
}

MessageContext::MessageContext() {
    msg_queue = new MessageQueue;
}

MessageContext::~MessageContext() {
    delete msg_queue;
}

void MessageContext::Start() {
    if (started_) {
        return;
    }
    started_ = true;
    msg_queue->Init();
    msg_queue->Start();
    thread_ = new std::thread(&MessageContext::MessageThread, this);
}

void MessageContext::NotifyMsg(int what, int arg1, int arg2) {
    if (!started_) {
        av_log(nullptr, AV_LOG_WARNING, "failed to notify msg to MessageContext which not started.\n");
        return;
    }
    Message msg = {0};
    msg.what = what;
    msg.arg1 = arg1;
    msg.arg2 = arg2;
    msg.next = nullptr;
    msg_queue->Put(&msg);
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
    msg_queue->Abort();
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    delete thread_;
}
