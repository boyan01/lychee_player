#include "media/data_source.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "test.h"

namespace lychee {
namespace {

// Bytes 0, 1, 2, ... (mod 251) that a test can hold back: a read waits
// until |allowed| covers it or the source is interrupted.
class GatedSource : public DataSource {
 public:
  explicit GatedSource(size_t size) : size_(size) {}

  int64_t Read(int64_t offset, uint8_t* data, size_t size) override {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [&] {
      return interrupted_ || static_cast<size_t>(offset) < allowed_ ||
             static_cast<size_t>(offset) >= size_;
    });
    if (interrupted_) {
      return -1;
    }
    reads_++;
    if (static_cast<size_t>(offset) >= size_) {
      return 0;
    }
    const size_t end =
        std::min({size_, allowed_, static_cast<size_t>(offset) + size});
    for (size_t i = static_cast<size_t>(offset); i < end; ++i) {
      data[i - static_cast<size_t>(offset)] = static_cast<uint8_t>(i % 251);
    }
    read_end_ = std::max(read_end_, end);
    return static_cast<int64_t>(end - static_cast<size_t>(offset));
  }

  int64_t size() const override { return static_cast<int64_t>(size_); }
  std::string error() const override { return "interrupted"; }

  void Interrupt() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      interrupted_ = true;
    }
    condition_.notify_all();
  }
  void ClearInterrupt() override {
    std::lock_guard<std::mutex> lock(mutex_);
    interrupted_ = false;
  }

  void Allow(size_t bytes) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      allowed_ = bytes;
    }
    condition_.notify_all();
  }

  // Waits up to a second for reads to reach |bytes|.
  bool WaitReadTo(size_t bytes) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (read_end_ >= bytes) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }

  int reads() {
    std::lock_guard<std::mutex> lock(mutex_);
    return reads_;
  }

 private:
  const size_t size_;
  std::mutex mutex_;
  std::condition_variable condition_;
  size_t allowed_ = static_cast<size_t>(-1);
  size_t read_end_ = 0;
  int reads_ = 0;
  bool interrupted_ = false;
};

// NOLINTBEGIN(bugprone-throwing-static-initialization)
TEST(DataSource, MayOpenReference) {
  const std::string remote = "https://cdn.example/live/index.m3u8";
  EXPECT_TRUE(MayOpenReference(remote, "https://other.example/s.ts"));
  EXPECT_TRUE(MayOpenReference(remote, "http://cdn.example/s.ts"));
  EXPECT_TRUE(!MayOpenReference(remote, "file:///etc/hosts"));
  EXPECT_TRUE(!MayOpenReference(remote, "/etc/hosts"));
  EXPECT_TRUE(!MayOpenReference(remote, "data:text/plain,x"));

  const std::string local = "/Users/u/Downloads/movie.mp4";
  EXPECT_TRUE(MayOpenReference(local, "/Users/u/Downloads/movie.png"));
  EXPECT_TRUE(MayOpenReference(local, "file:///Users/u/a.png"));
  EXPECT_TRUE(MayOpenReference(local, "C:/Users/u/a.png"));
  EXPECT_TRUE(MayOpenReference(local, "https://cdn.example/a.png"));
  // UNC paths, which Windows would log in to.
  EXPECT_TRUE(!MayOpenReference(local, "//attacker.example/share/a.png"));
  EXPECT_TRUE(!MayOpenReference(local, "\\\\attacker.example\\share\\a"));
  EXPECT_TRUE(!MayOpenReference(local, "/\\attacker.example/share/a.png"));
  EXPECT_TRUE(!MayOpenReference(local, "file:////attacker.example/s/a.ts"));
  EXPECT_TRUE(!MayOpenReference(local, "\\\\?\\UNC\\attacker\\s\\a"));
  EXPECT_TRUE(!MayOpenReference(local, "\\\\.\\pipe\\p"));
  // Media on a share may use that share, under any spelling of its host.
  const std::string share = "//NAS/media/show/index.m3u8";
  EXPECT_TRUE(MayOpenReference(share, "//nas/media/show/s.ts"));
  EXPECT_TRUE(MayOpenReference("\\\\nas\\media\\a.mp4", "//NAS/media/a.png"));
  EXPECT_TRUE(!MayOpenReference(share, "//attacker.example/media/s.ts"));
  // An Android content provider's media: the network and that provider.
  const std::string provider = "content://com.example.files/hls/index.m3u8";
  EXPECT_TRUE(MayOpenReference(provider, "content://com.example.files/hls/s"));
  EXPECT_TRUE(MayOpenReference(provider, "https://cdn.example/s.ts"));
  EXPECT_TRUE(!MayOpenReference(provider, "content://com.example.filesx/s"));
  EXPECT_TRUE(!MayOpenReference(provider, "content://other/hls/s.ts"));
  EXPECT_TRUE(!MayOpenReference(provider, "/data/data/app/secret"));
  EXPECT_TRUE(!MayOpenReference(provider, "file:///sdcard/a.ts"));
  EXPECT_TRUE(!MayOpenReference("content://bare", "content://bare/a"));
}

TEST(PrefetchingDataSource, ReadsAheadThenPassesOn) {
  auto gated = std::make_unique<GatedSource>(300000);
  GatedSource* inner = gated.get();
  auto source = CreatePrefetchingDataSource(std::move(gated), 200000);
  // The first 200000 bytes arrive with nobody reading.
  EXPECT_TRUE(inner->WaitReadTo(200000));
  const int reads = inner->reads();
  std::vector<uint8_t> data(300000);
  size_t at = 0;
  while (at < data.size()) {
    const int64_t n = source->Read(static_cast<int64_t>(at), data.data() + at,
                                   data.size() - at);
    if (n <= 0) {
      break;
    }
    at += static_cast<size_t>(n);
  }
  EXPECT_EQ(at, data.size());
  bool same = true;
  for (size_t i = 0; i < data.size(); ++i) {
    same &= data[i] == static_cast<uint8_t>(i % 251);
  }
  EXPECT_TRUE(same);
  // What was fetched came from memory; the rest from the source.
  EXPECT_TRUE(inner->reads() > reads);
  uint8_t byte = 0;
  EXPECT_EQ(source->Read(300000, &byte, 1), 0);
  EXPECT_EQ(source->size(), 300000);
}

TEST(PrefetchingDataSource, ReadWaitsForTheThread) {
  auto gated = std::make_unique<GatedSource>(1000);
  GatedSource* inner = gated.get();
  inner->Allow(0);
  auto source = CreatePrefetchingDataSource(std::move(gated), 1000);
  std::thread release([inner] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    inner->Allow(1000);
  });
  uint8_t data[16] = {};
  EXPECT_EQ(source->Read(500, data, sizeof(data)), 16);
  EXPECT_EQ(data[0], static_cast<uint8_t>(500 % 251));
  release.join();
}

TEST(PrefetchingDataSource, Interrupt) {
  auto gated = std::make_unique<GatedSource>(1000);
  GatedSource* inner = gated.get();
  inner->Allow(0);
  auto source = CreatePrefetchingDataSource(std::move(gated), 1000);
  std::atomic<int64_t> result{1};
  std::thread reader([&] {
    uint8_t byte = 0;
    result = source->Read(0, &byte, 1);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  source->Interrupt();
  reader.join();
  EXPECT_EQ(result.load(), -1);
  EXPECT_TRUE(source->interrupted());
  // After the interrupt the thread has stopped; reads go to the source.
  source->ClearInterrupt();
  inner->Allow(1000);
  uint8_t byte = 0;
  EXPECT_EQ(source->Read(10, &byte, 1), 1);
  EXPECT_EQ(byte, 10);
}

TEST(PrefetchingDataSource, DestroyedWhileReading) {
  auto gated = std::make_unique<GatedSource>(1000);
  gated->Allow(0);
  // The thread is stuck in the source's read: destruction interrupts it.
  auto source = CreatePrefetchingDataSource(std::move(gated), 1000);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  source.reset();
  EXPECT_TRUE(source == nullptr);
}
TEST(DataSource, NormalizeHttpHeaders) {
  std::string headers;
  EXPECT_TRUE(NormalizeHttpHeaders("", &headers));
  EXPECT_EQ(headers, "");
  EXPECT_TRUE(NormalizeHttpHeaders(
      "User-Agent: tv/1.0\nReferer:https://a.example/ \r\n\r\nX-A:", &headers));
  EXPECT_EQ(headers,
            "User-Agent: tv/1.0\r\nReferer: https://a.example/\r\nX-A: \r\n");
  const auto pairs = SplitHttpHeaders(headers);
  EXPECT_EQ(pairs.size(), 3u);
  EXPECT_EQ(pairs[1].first, "Referer");
  EXPECT_EQ(pairs[1].second, "https://a.example/");
  EXPECT_EQ(pairs[2].second, "");
  // No name, no colon, a name that is not a token, a stray CR that would
  // start a header of its own.
  EXPECT_TRUE(!NormalizeHttpHeaders(": value", &headers));
  EXPECT_TRUE(!NormalizeHttpHeaders("Referer", &headers));
  EXPECT_TRUE(!NormalizeHttpHeaders("Bad Name: value", &headers));
  EXPECT_TRUE(!NormalizeHttpHeaders("Cookie: a=1\rHost: evil", &headers));
}

// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace
}  // namespace lychee
