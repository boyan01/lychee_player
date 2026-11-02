#include "media/buffer_pool.h"

#include <mutex>
#include <new>
#include <vector>

namespace lychee {

namespace {

constexpr std::align_val_t kAlignment{64};

void Free(uint8_t* buffer) {
  ::operator delete[](buffer, kAlignment);
}

}  // namespace

struct BufferPool::Shared {
  ~Shared() {
    for (uint8_t* buffer : free) {
      Free(buffer);
    }
  }

  std::mutex mutex;
  size_t size = 0;  // Of the buffers in |free|.
  std::vector<uint8_t*> free;
};

BufferPool::BufferPool() : shared_(std::make_shared<Shared>()) {}

BufferPool::~BufferPool() = default;

std::shared_ptr<uint8_t> BufferPool::Get(size_t size) {
  uint8_t* buffer = nullptr;
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (size != shared_->size) {
      for (uint8_t* old : shared_->free) {
        Free(old);
      }
      shared_->free.clear();
      shared_->size = size;
    } else if (!shared_->free.empty()) {
      buffer = shared_->free.back();
      shared_->free.pop_back();
    }
  }
  if (!buffer) {
    buffer =
        static_cast<uint8_t*>(::operator new[](size, kAlignment, std::nothrow));
    if (!buffer) {
      return nullptr;
    }
  }
  return std::shared_ptr<uint8_t>(
      buffer, [shared = shared_, size](uint8_t* returned) {
        {
          std::lock_guard<std::mutex> lock(shared->mutex);
          if (size == shared->size) {
            shared->free.push_back(returned);
            return;
          }
        }
        Free(returned);
      });
}

}  // namespace lychee
