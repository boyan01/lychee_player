//
// Created by yangbin on 2021/3/28.
//

#include <gtest/gtest.h>

#include "logging.h"
#include "ffmpeg_common.h"

namespace media {

static AVIndexEntry kIndexEntries[] = {
    // pos, timestamp, flags, size, min_distance
    {0, 0, AVINDEX_KEYFRAME, 0, 0},
    {2000, 1000, AVINDEX_KEYFRAME, 0, 0},
    {3000, 2000, 0, 0, 0},
    {5000, 3000, AVINDEX_KEYFRAME, 0, 0},
    {6000, 4000, 0, 0, 0},
    {8000, 5000, AVINDEX_KEYFRAME, 0, 0},
    {9000, 6000, AVINDEX_KEYFRAME, 0, 0},
    {11500, 7000, AVINDEX_KEYFRAME, 0, 0},
};

static const AVRational kTimeBase = {1, 1000};

class FFmpegCommonTest : public testing::Test {
 public:
  FFmpegCommonTest();
  virtual ~FFmpegCommonTest();

 protected:
  AVStream stream_;

  DISALLOW_COPY_AND_ASSIGN(FFmpegCommonTest);
};

FFmpegCommonTest::FFmpegCommonTest() {
  stream_.time_base = kTimeBase;
  stream_.index_entries = kIndexEntries;
  stream_.index_entries_allocated_size = sizeof(kIndexEntries);
  stream_.nb_index_entries = arraysize(kIndexEntries);
}

FFmpegCommonTest::~FFmpegCommonTest() = default;

TEST_F(FFmpegCommonTest, TestTimeBaseConversions) {
  int64 test_data[][5] = {
      {1, 2, 1, 500000, 1},
      {1, 3, 1, 333333, 1},
      {1, 3, 2, 666667, 2},
  };

  for (size_t i = 0; i < arraysize(test_data); ++i) {
    SCOPED_TRACE(i);

    AVRational time_base;
    time_base.num = static_cast<int>(test_data[i][0]);
    time_base.den = static_cast<int>(test_data[i][1]);

    chrono::microseconds time_delta = ConvertFromTimeBase(time_base, test_data[i][2]);

    EXPECT_EQ(time_delta.count(), test_data[i][3]);
    EXPECT_EQ(ConvertToTimeBase(time_base, time_delta), test_data[i][4]);
  }
}

}  // namespace media