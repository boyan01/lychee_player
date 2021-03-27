//
// Created by boyan on 2021/3/27.
//

#include "logging.h"
#include "message_loop.h"

namespace media {

namespace base {

void MessageLoop::PostTask(const tracked_objects::Location &from_here, TaskClosure &task) {
  DCHECK(!task) << from_here.ToString();
  Message message(task, from_here);
  AddToIncomingQueue(&message);
}

void MessageLoop::AddToIncomingQueue(Message *message) {

}

void MessageLoop::DoWork() {

}

void MessageLoop::ReloadWorkQueue() {


}

void MessageLoop::AddToDelayedWorkQueue(const Message &message) {
  // Move to the delayed work queue.
}

void MessageLoop::RunTask(const Message &message) {
  message.task();
}

} // namespace base
} // namespace media