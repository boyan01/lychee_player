//
// Created by yangbin on 2021/4/5.
//

#ifndef MEDIA_PLAYER_SOURCE_FILE_DATA_SOURCE_H_
#define MEDIA_PLAYER_SOURCE_FILE_DATA_SOURCE_H_

#include "fstream"

#include "data_source.h"

namespace media {

// Basic data source that treats the URL as a file path, and uses the file
// system to read data for a media pipeline.
class FileDataSource : public DataSource {

 public:
  FileDataSource();
  ~FileDataSource() override;

  bool Initialize(const std::string &file_path);

  // Implementation of DataSource.
  void Stop() override;
  void Abort() override;
  void Read(int64_t position,
            int size,
            uint8_t *data,
            DataSource::ReadCB read_cb) override;
  bool GetSize(int64_t *size_out) override;
  bool IsStreaming() override;
  void SetBitrate(int bitrate) override;

  // Unit test helpers. Recreate the object if you want the default behaviour.
  void force_read_errors_for_testing() { force_read_errors_ = true; }
  void force_streaming_for_testing() { force_streaming_ = true; }
  uint64_t bytes_read_for_testing() { return bytes_read_; }
  void reset_bytes_read_for_testing() { bytes_read_ = 0; }

 private:

  std::ifstream file_stream_;

  bool force_read_errors_;
  bool force_streaming_;
  uint64_t bytes_read_;

  DELETE_COPY_AND_ASSIGN(FileDataSource);

};

} // namespace media
#endif //MEDIA_PLAYER_SOURCE_FILE_DATA_SOURCE_H_
