//
// Created by yangbin on 2021/4/5.
//

#include "file_data_source.h"

#include "base/logging.h"

namespace media {

FileDataSource::FileDataSource()
    : force_read_errors_(false),
      force_streaming_(false),
      bytes_read_(0),
      file_(nullptr) {}

bool FileDataSource::Initialize(const std::string &file_path) {
  DCHECK(!file_);
  file_ = fopen(file_path.c_str(), "rb");
  return file_;
}

void FileDataSource::Stop() {}

void FileDataSource::Abort() {}

void FileDataSource::Read(int64_t position, int size, uint8_t *data,
                          DataSource::ReadCB read_cb) {
  if (force_read_errors_ || !file_) {
    std::move(read_cb)(kReadError);
    return;
  }

  int64_t file_size;
  GetSize(&file_size);

  CHECK_GE(file_size, 0);
  CHECK_GE(position, 0);
  CHECK_GE(size, 0);

  // Cap position and size within bounds.
  position = std::min(position, file_size);
  int64_t clamped_size =
      std::min(static_cast<int64_t>(size), file_size - position);

  auto ret = fseek(file_, position, SEEK_SET);
  DCHECK_EQ(ret, 0);
  fread(data, sizeof(char), clamped_size, file_);

  bytes_read_ += clamped_size;
  std::move(read_cb)(clamped_size);
}

bool FileDataSource::GetSize(int64_t *size_out) {
  if (!file_) {
    return false;
  }
  auto ret = fseek(file_, 0, SEEK_END);
  if (ret != 0) {
    return false;
  }
  *size_out = ftell(file_);
  return true;
}

bool FileDataSource::IsStreaming() { return force_streaming_; }

void FileDataSource::SetBitrate(int bitrate) {}

FileDataSource::~FileDataSource() = default;

}  // namespace media