#ifndef LYCHEE_MEDIA_BUFFER_POOL_H_
#define LYCHEE_MEDIA_BUFFER_POOL_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lychee {

// Recycles the pixel buffers of CPU frames. They are megabytes each (33 MB
// at 4K), so a fresh one per frame costs page faults and the kernel's
// zeroing of every page, frame after frame. A buffer returns to the pool
// when its last reference goes, on whatever thread that is; the pool keeps
// what comes back while it is asked for the same size, and drops it all
// when the size changes. Buffers may outlive the pool.
class BufferPool {
 public:
  BufferPool();
  ~BufferPool();
  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // |size| bytes, 64-byte aligned (for SIMD), with undefined contents; null
  // when out of memory.
  std::shared_ptr<uint8_t> Get(size_t size);

 private:
  struct Shared;
  std::shared_ptr<Shared> shared_;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_BUFFER_POOL_H_
