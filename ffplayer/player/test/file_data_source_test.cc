//
// Created by yangbin on 2021/5/12.
//

#include "file_data_source.h"

#include "gtest/gtest.h"

namespace media {

const char *test_file_name = "/Users/yangbin/Pictures/mojito.mp4";

TEST(FileDataSourceTest, GetFileSize) {
  auto source = new FileDataSource();
  source->Initialize(test_file_name);
  int64 size;
  source->GetSize(&size);

  EXPECT_EQ(size, 40431076);
}

TEST(FileDataSourceTest, CopyFile) {
  auto source = new FileDataSource();
  source->Initialize(test_file_name);

  // TODO.
}

}  // namespace media
