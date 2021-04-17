//
// Created by yangbin on 2021/4/4.
//

#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "ffmpeg_demuxer.h"

namespace media {

MATCHER(IsEndOfStreamBuffer, std::string(negation ? "isn't" : "is") + "end of stream") {
  return arg->IsEndOfStream();
}

class FFmpegDemuxerTest : public testing::Test {
 protected:
  FFmpegDemuxerTest() = default;

  virtual ~FFmpegDemuxerTest() {
    if (demuxer_) {
//      demuxer_->Stop();
    }
  }

  std::shared_ptr<FFmpegDemuxer> demuxer_;

};

}