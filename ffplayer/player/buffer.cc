//
// Created by yangbin on 2021/3/28.
//

#include "buffer.h"

namespace media {

Buffer::Buffer(const chrono::milliseconds &timestamp, const chrono::milliseconds &duration)
    : timestamp_(timestamp), duration_(duration) {}

Buffer::~Buffer() = default;

bool Buffer::isEndOfStream() const {
  return GetData() == nullptr;
}

}