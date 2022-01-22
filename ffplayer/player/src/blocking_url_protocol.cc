// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "blocking_url_protocol.h"

#include <utility>

#include "base/logging.h"
#include "data_source.h"

namespace media {

BlockingUrlProtocol::BlockingUrlProtocol(DataSource *data_source,
                                         std::function<void()> error_cb)
    : data_source_(data_source),
      error_cb_(std::move(error_cb)),
      is_streaming_(data_source_->IsStreaming()),
      last_read_bytes_(0),
      read_position_(0),
      aborted_(false),
      read_condition_() {}

BlockingUrlProtocol::~BlockingUrlProtocol() = default;

void BlockingUrlProtocol::Abort() {
  std::lock_guard<std::mutex> lock(data_source_lock_);
  aborted_ = true;
  read_condition_.notify_one();
  data_source_ = nullptr;
}

int BlockingUrlProtocol::Read(int size, uint8_t *data) {
  {
    // Read errors are unrecoverable.
    std::lock_guard<std::mutex> lock(data_source_lock_);
    if (!data_source_) {
      DCHECK(aborted_);
      return AVERROR(EIO);
    }

    // Not sure this can happen, but it's unclear from the ffmpeg code, so guard
    // against it.
    if (size < 0) return AVERROR(EIO);
    if (!size) return 0;

    int64_t file_size;
    if (data_source_->GetSize(&file_size) && read_position_ >= file_size)
      return AVERROR_EOF;

    // Blocking read from data source until either:
    //   1) |last_read_bytes_| is set and |read_complete_| is signalled
    //   2) |aborted_| is signalled
    data_source_->Read(read_position_, size, data,
                       [this](int size) { SignalReadCompleted(size); });
  }

  {
    std::unique_lock<std::mutex> lock(data_source_lock_);
    read_condition_.wait(lock, [this] { return aborted_ || read_complete_; });
    read_complete_ = false;
  }

  if (aborted_) return AVERROR(EIO);

  if (last_read_bytes_ == DataSource::kReadError) {
    aborted_ = true;
    error_cb_();
    return AVERROR(EIO);
  }

  if (last_read_bytes_ == DataSource::kAborted) return AVERROR(EIO);

  read_position_ += last_read_bytes_;
  return last_read_bytes_;
}

bool BlockingUrlProtocol::GetPosition(int64_t *position_out) {
  *position_out = read_position_;
  return true;
}

bool BlockingUrlProtocol::SetPosition(int64_t position) {
  std::lock_guard<std::mutex> lock(data_source_lock_);

  int64_t file_size;
  if (!data_source_ ||
      (data_source_->GetSize(&file_size) && position > file_size) ||
      position < 0) {
    return false;
  }

  read_position_ = position;
  return true;
}

bool BlockingUrlProtocol::GetSize(int64_t *size_out) {
  std::lock_guard<std::mutex> lock(data_source_lock_);
  return data_source_ ? data_source_->GetSize(size_out) : 0;
}

bool BlockingUrlProtocol::IsStreaming() { return is_streaming_; }

void BlockingUrlProtocol::SignalReadCompleted(int size) {
  last_read_bytes_ = size;
  read_complete_ = true;
  read_condition_.notify_one();
}

}  // namespace media
