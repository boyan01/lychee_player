// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_FILTERS_BLOCKING_URL_PROTOCOL_H_
#define MEDIA_FILTERS_BLOCKING_URL_PROTOCOL_H_

#include "base/basictypes.h"
#include "condition_variable"
#include "ffmpeg_glue.h"
#include "functional"
#include "mutex"

namespace media {

class DataSource;

// An implementation of FFmpegURLProtocol that blocks until the underlying
// asynchronous DataSource::Read() operation completes. Generally constructed on
// the media thread and used by ffmpeg through the AVIO interface from a
// sequenced blocking pool.
class BlockingUrlProtocol : public FFmpegURLProtocol {
 public:
  // Implements FFmpegURLProtocol using the given |data_source|. |error_cb| is
  // fired any time DataSource::Read() returns an error.
  BlockingUrlProtocol(DataSource *data_source, std::function<void()> error_cb);
  virtual ~BlockingUrlProtocol();

  // Aborts any pending reads by returning a read error. After this method
  // returns all subsequent calls to Read() will immediately fail. May be called
  // from any thread and upon return ensures no further use of |data_source_|.
  void Abort();

  // FFmpegURLProtocol implementation.
  int Read(int size, uint8_t *data) override;
  bool GetPosition(int64_t *position_out) override;
  bool SetPosition(int64_t position) override;
  bool GetSize(int64_t *size_out) override;
  bool IsStreaming() override;

 private:
  // Sets |last_read_bytes_| and signals the blocked thread that the read
  // has completed.
  void SignalReadCompleted(int size);

  // |data_source_lock_| allows Abort() to be called from any thread and stop
  // all outstanding access to |data_source_|. Typically Abort() is called from
  // the media thread while ffmpeg is operating on another thread.
  std::mutex data_source_lock_;
  DataSource *data_source_;

  std::function<void(void)> error_cb_;
  const bool is_streaming_;

  // Cached number of bytes last read from the data source.
  int last_read_bytes_;

  // Cached position within the data source.
  int64_t read_position_;

  bool aborted_;
  bool read_complete_;
  std::condition_variable_any read_condition_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(BlockingUrlProtocol);
};

}  // namespace media

#endif  // MEDIA_FILTERS_BLOCKING_URL_PROTOCOL_H_
