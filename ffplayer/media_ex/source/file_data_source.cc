//
// Created by yangbin on 2021/4/5.
//

#include "base/logging.h"
#include "source/file_data_source.h"

namespace media {

FileDataSource::FileDataSource()
    : force_read_errors_(false),
      force_streaming_(false),
      bytes_read_(0) {
}

bool FileDataSource::Initialize(const std::string &file_path) {
  DCHECK(!file_stream_.is_open());
  file_stream_ = std::ifstream(file_path, std::ios::binary);
  return file_stream_.is_open();
}

void FileDataSource::Stop() {}

void FileDataSource::Abort() {}

void FileDataSource::Read(int64_t position,
                          int size,
                          uint8_t *data,
                          DataSource::ReadCB read_cb) {
  if (force_read_errors_ || !file_stream_.is_open()) {
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

  file_stream_.seekg(position);
  file_stream_.read(reinterpret_cast<char *>(data), clamped_size);

  bytes_read_ += clamped_size;
  std::move(read_cb)(clamped_size);
}

bool FileDataSource::GetSize(int64_t *size_out) {
  if (file_stream_.is_open()) {
    return false;
  }
  file_stream_.seekg(0, std::ios::end);
  *size_out = file_stream_.tellg();
  return true;
}

bool FileDataSource::IsStreaming() {
  return force_streaming_;
}

void FileDataSource::SetBitrate(int bitrate) {}

FileDataSource::~FileDataSource() = default;

} // namespace media