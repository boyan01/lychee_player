#include "media/buffer_pool.h"

#include <cstring>
#include <thread>

#include "test.h"

namespace lychee {
namespace {

// Test registration allocates before main, as all TEST declarations do.
// NOLINTBEGIN(bugprone-throwing-static-initialization)
TEST(BufferPool, ReusesReturnedBuffers) {
  BufferPool pool;
  std::shared_ptr<uint8_t> first = pool.Get(1000);
  EXPECT_TRUE(first != nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(first.get()) % 64, 0u);
  std::memset(first.get(), 1, 1000);
  uint8_t* address = first.get();
  // Still held: a second buffer.
  std::shared_ptr<uint8_t> second = pool.Get(1000);
  EXPECT_TRUE(second.get() != address);
  first.reset();
  EXPECT_TRUE(pool.Get(1000).get() == address);
}

TEST(BufferPool, DropsBuffersOfAnotherSize) {
  BufferPool pool;
  std::shared_ptr<uint8_t> small = pool.Get(64);
  std::shared_ptr<uint8_t> large = pool.Get(4096);
  // Returned after the size changed: freed, not kept.
  small.reset();
  std::shared_ptr<uint8_t> next = pool.Get(4096);
  EXPECT_TRUE(next != nullptr);
  uint8_t* address = large.get();
  large.reset();
  EXPECT_TRUE(pool.Get(4096).get() == address);
}

TEST(BufferPool, BuffersOutliveThePool) {
  std::shared_ptr<uint8_t> buffer;
  {
    BufferPool pool;
    buffer = pool.Get(256);
  }
  // AddressSanitizer would catch a use after free or a leak.
  std::memset(buffer.get(), 2, 256);
  buffer.reset();
}

TEST(BufferPool, ReturnsOnAnyThread) {
  BufferPool pool;
  std::shared_ptr<uint8_t> buffer = pool.Get(512);
  uint8_t* address = buffer.get();
  std::thread([&buffer] { buffer.reset(); }).join();
  EXPECT_TRUE(pool.Get(512).get() == address);
}
// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace
}  // namespace lychee
