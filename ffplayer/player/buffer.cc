//
// Created by yangbin on 2021/3/28.
//

#include "buffer.h"

namespace media {

Buffer::Buffer(const TimeDelta &timestamp, const TimeDelta &duration)
    : timestamp_(timestamp), duration_(duration) {}

Buffer::~Buffer() = default;

bool Buffer::isEndOfStream() const {
  return GetData() == nullptr;
}

}