#include "backends/ffmpeg/ffmpeg_data_source.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <mutex>
#include <vector>

#include "backends/ffmpeg/ffmpeg_common.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

namespace lychee {

namespace {

// A read this far ahead of the last one reads on through the request
// instead of starting a range request, as the system clients stream past
// it (backends/apple/http_data_source.mm): TsDemuxer samples timestamps
// every few hundred KB at open.
constexpr int64_t kSkipAhead = 1 << 20;
// At least this much of what was read stays for reads back into it.
constexpr size_t kKeepBehind = 1 << 20;

class FFmpegHttpDataSource : public DataSource {
 public:
  explicit FFmpegHttpDataSource(std::string url) : url_(std::move(url)) {}
  ~FFmpegHttpDataSource() override { avio_closep(&io_); }

  int64_t Read(int64_t offset, uint8_t* data, size_t size) override {
    if (interrupted_.load()) {
      return -1;
    }
    if (!io_ && !Connect()) {
      return -1;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (size_ >= 0 && offset >= size_) {
        return 0;
      }
    }
    if (offset >= behind_start_ && offset < position_) {
      const size_t n =
          std::min<size_t>(size, static_cast<size_t>(position_ - offset));
      const auto from = behind_.begin() + (offset - behind_start_);
      std::copy(from, from + static_cast<std::ptrdiff_t>(n), data);
      return static_cast<int64_t>(n);
    }
    if (offset != position_ && !SeekTo(offset)) {
      return -1;
    }
    return ReadIo(data, size);
  }

  int64_t size() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
  }

  bool seekable() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return seekable_;
  }

  std::string url() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return location_;
  }

  void Interrupt() override { interrupted_ = true; }
  void ClearInterrupt() override { interrupted_ = false; }
  bool interrupted() const override { return interrupted_.load(); }

  std::string error() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

 private:
  static int InterruptCallback(void* opaque) {
    return static_cast<FFmpegHttpDataSource*>(opaque)->interrupted_.load() ? 1
                                                                           : 0;
  }

  // Opens a request from the start.
  bool Connect() {
    avio_closep(&io_);
    position_ = behind_start_ = 0;
    behind_.clear();
    AVDictionary* options = nullptr;
    // As FFmpegDemuxer::Open() sets them for FFmpeg's http.
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_on_network_error", "1", 0);
    av_dict_set(&options, "reconnect_on_http_error", "5xx", 0);
    av_dict_set(&options, "reconnect_delay_max", "4", 0);
    av_dict_set(&options, "reconnect_delay_total_max", "10", 0);
    av_dict_set(&options, "rw_timeout", "10000000", 0);
    const AVIOInterruptCB interrupt = {&InterruptCallback, this};
    const int ret =
        avio_open2(&io_, url_.c_str(), AVIO_FLAG_READ, &interrupt, &options);
    av_dict_free(&options);
    if (ret < 0) {
      SetError(url_ + ": " + AVErrorString(ret));
      return false;
    }
    const int64_t size = avio_size(io_);
    char* location = nullptr;
    av_opt_get(io_, "location", AV_OPT_SEARCH_CHILDREN,
               reinterpret_cast<uint8_t**>(&location));
    std::lock_guard<std::mutex> lock(mutex_);
    size_ = size >= 0 ? size : -1;
    seekable_ = (io_->seekable & AVIO_SEEKABLE_NORMAL) != 0;
    location_ = location && url_ != location ? location : "";
    av_free(location);
    return true;
  }

  // Reads at |position_|, keeping what it read behind (|behind_|).
  int64_t ReadIo(uint8_t* data, size_t size) {
    const int ret =
        avio_read(io_, data, static_cast<int>(std::min<size_t>(size, INT_MAX)));
    if (ret == AVERROR_EOF) {
      return 0;
    }
    if (ret < 0) {
      SetError("read: " + AVErrorString(ret));
      return -1;
    }
    behind_.insert(behind_.end(), data, data + ret);
    position_ += ret;
    // Trimmed in steps, so the move is paid once per kKeepBehind read.
    if (behind_.size() > 2 * kKeepBehind) {
      const size_t drop = behind_.size() - kKeepBehind;
      behind_.erase(behind_.begin(),
                    behind_.begin() + static_cast<std::ptrdiff_t>(drop));
      behind_start_ += static_cast<int64_t>(drop);
    }
    return ret;
  }

  // Reads on to |offset| when it is a little ahead; else a range request,
  // or where the server takes none, the request again from the start,
  // read up to |offset|.
  bool SeekTo(int64_t offset) {
    const bool near = offset > position_ && offset - position_ <= kSkipAhead;
    if (!near && seekable()) {
      const int64_t ret = avio_seek(io_, offset, SEEK_SET);
      if (ret < 0) {
        SetError("seek: " + AVErrorString(static_cast<int>(ret)));
        return false;
      }
      position_ = behind_start_ = offset;
      behind_.clear();
      return true;
    }
    if (offset < position_ && !Connect()) {
      return false;
    }
    uint8_t skipped[64 << 10];
    while (position_ < offset) {
      const int64_t ret =
          ReadIo(skipped, static_cast<size_t>(std::min<int64_t>(
                              offset - position_, sizeof(skipped))));
      if (ret <= 0) {
        if (ret == 0) {
          SetError("read: unexpected end of data");
        }
        return false;
      }
    }
    return true;
  }

  void SetError(std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::move(error);
  }

  const std::string url_;
  // Read() only.
  AVIOContext* io_ = nullptr;
  int64_t position_ = 0;
  // The bytes before |position_| from |behind_start_| on: a read a little
  // back (a parser that looks again) needs no request.
  std::vector<uint8_t> behind_;
  int64_t behind_start_ = 0;
  std::atomic<bool> interrupted_{false};

  mutable std::mutex mutex_;
  int64_t size_ = -1;
  bool seekable_ = false;
  std::string location_;
  std::string error_;
};

}  // namespace

std::unique_ptr<DataSource> OpenFFmpegHttpDataSource(const std::string& url,
                                                     std::string* /*error*/) {
  return std::make_unique<FFmpegHttpDataSource>(url);
}

}  // namespace lychee
