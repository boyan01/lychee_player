//
// Created by boyan on 2021/2/6.
//

#include "ffp_msg_queue.h"

extern "C" {
#include "libavutil/common.h"
}


int FFPlayerMessageQueue::Init() {
    memset(this, 0, sizeof(FFPlayerMessageQueue));
    mutex_ = new std::mutex;
    cond_ = new std::condition_variable_any;
    abort_request_ = 1;
    return 0;
}

int FFPlayerMessageQueue::Put(FFPlayerMessage *msg) {
    int ret;
    mutex_->lock();
    ret = PutPrivate(msg);
    mutex_->unlock();
    return ret;
}

int FFPlayerMessageQueue::PutPrivate(FFPlayerMessage *msg) {
    FFPlayerMessage *msg1;

    if (abort_request_)
        return -1;

    msg1 = (FFPlayerMessage *) av_malloc(sizeof(FFPlayerMessage));
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

void FFPlayerMessageQueue::Flush() {
    FFPlayerMessage *msg, *msg1;

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

void FFPlayerMessageQueue::Destroy() {
    Flush();
    delete mutex_;
    delete cond_;
}

void FFPlayerMessageQueue::Abort() {
    mutex_->lock();

    abort_request_ = 1;

    cond_->notify_all();
    mutex_->unlock();
}

void FFPlayerMessageQueue::Start() {
    mutex_->lock();
    abort_request_ = 0;
    FFPlayerMessage msg = {0};
    msg.what = FFP_MSG_FLUSH;
    PutPrivate(&msg);
    mutex_->unlock();
}

int FFPlayerMessageQueue::Get(FFPlayerMessage *msg, int block) {
    FFPlayerMessage *msg1;
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

void FFPlayerMessageQueue::Remove(int what) {
    FFPlayerMessage *p_msg, *msg, *last_msg;
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
