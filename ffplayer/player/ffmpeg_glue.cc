//
// Created by yangbin on 2021/4/2.
//

#include <strstream>

#include "base/logging.h"

#include "ffmpeg_glue.h"
#include "ffmpeg_common.h"

namespace media {

static FFmpegUrlProtocol *ToProtocol(void *data) {
  return reinterpret_cast<FFmpegUrlProtocol *>(data);
}

// FFmpeg protocol interface.
static int OpenContext(URLContext *h, const char *filename, int flags) {
  FFmpegUrlProtocol *protocol;
  FFmpegGlue::GetInstance()->GetProtocol(filename, &protocol);
  if (!protocol)
    return AVERROR(EIO);

  h->priv_data = protocol;
  h->flags = AVIO_FLAG_READ;
  h->is_streamed = protocol->IsStreaming();
  return 0;
}

static int ReadContext(URLContext *h, unsigned char *buf, int size) {
  FFmpegUrlProtocol *protocol = ToProtocol(h->priv_data);
  int result = protocol->Read(size, buf);
  if (result < 0)
    result = AVERROR(EIO);
  return result;
}

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(52, 68, 0)
static int WriteContext(URLContext *h, const unsigned char *buf, int size) {
#else
  static int WriteContext(URLContext* h, unsigned char* buf, int size) {
#endif
  // We don't support writing.
  return AVERROR(EIO);
}

static int64 SeekContext(URLContext *h, int64 offset, int whence) {
  FFmpegUrlProtocol *protocol = ToProtocol(h->priv_data);
  int64 new_offset = AVERROR(EIO);
  switch (whence) {
    case SEEK_SET:
      if (protocol->SetPosition(offset))
        protocol->GetPosition(&new_offset);
      break;

    case SEEK_CUR:int64 pos;
      if (!protocol->GetPosition(&pos))
        break;
      if (protocol->SetPosition(pos + offset))
        protocol->GetPosition(&new_offset);
      break;

    case SEEK_END:int64 size;
      if (!protocol->GetSize(&size))
        break;
      if (protocol->SetPosition(size + offset))
        protocol->GetPosition(&new_offset);
      break;

    case AVSEEK_SIZE:protocol->GetSize(&new_offset);
      break;

    default:NOTREACHED();
  }
  if (new_offset < 0)
    new_offset = AVERROR(EIO);
  return new_offset;
}

static int CloseContext(URLContext *h) {
  h->priv_data = nullptr;
  return 0;
}

static int LockManagerOperation(void **lock, enum AVLockOp op) {
  switch (op) {
    case AV_LOCK_CREATE: {
      *lock = new std::mutex();
      if (!*lock)
        return 1;
      return 0;
    }
    case AV_LOCK_OBTAIN: {
      static_cast<std::mutex *>(*lock)->lock();
      return 0;
    }
    case AV_LOCK_RELEASE: {
      static_cast<std::mutex *>(*lock)->unlock();
      return 0;
    }
    case AV_LOCK_DESTROY: {
      delete static_cast<std::mutex *>(*lock);
      *lock = nullptr;
      return 0;
    }
  }
  return 1;
}

// Use the HTTP protocol to avoid any file path separator issues.
static const char kProtocol[] = "http";

// Fill out our FFmpeg protocol definition.
static URLProtocol kFFmpegUrlProtocol = {
    kProtocol,
    &OpenContext,
    nullptr,  // url_open2
    nullptr, // url_accept
    nullptr, // url_handshake
    &ReadContext,
    &WriteContext,
    &SeekContext,
    &CloseContext,
};

// static
FFmpegGlue *FFmpegGlue::GetInstance() {
  static FFmpegGlue glue;
  return &glue;
}

// static
URLProtocol *FFmpegGlue::url_protocol() {
  return &kFFmpegUrlProtocol;
}

std::string FFmpegGlue::AddProtocol(FFmpegUrlProtocol *protocol) {
  std::lock_guard<std::mutex> auto_lock(mutex_);
  auto key = GetProtocolKey(protocol);
  if (protocols_.find(key) == protocols_.end()) {
    protocols_[key] = protocol;
  }
  return key;
}

void FFmpegGlue::RemoveProtocol(FFmpegUrlProtocol *protocol) {
  std::lock_guard<std::mutex> auto_lock(mutex_);
  for (ProtocolMap::iterator cur, iter = protocols_.begin(); iter != protocols_.end();) {
    cur = iter;
    iter++;
    if (cur->second == protocol) {
      protocols_.erase(cur);
    }
  }
}

void FFmpegGlue::GetProtocol(const std::string &key, FFmpegUrlProtocol **protocol) {
  std::lock_guard<std::mutex> auto_lock(mutex_);
  auto iter = protocols_.find(key);
  if (iter == protocols_.end()) {
    *protocol = nullptr;
    return;
  }
  *protocol = iter->second;
}

FFmpegGlue::FFmpegGlue() = default;
FFmpegGlue::~FFmpegGlue() = default;

std::string FFmpegGlue::GetProtocolKey(FFmpegUrlProtocol *protocol) {
  std::stringstream str_stream;
  str_stream << kProtocol << "://" << protocol;
  return str_stream.str();
}

}