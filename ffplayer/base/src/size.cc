//
// Created by yangbin on 2021/3/28.
//

#include "base/size.h"
#include "base/logging.h"

namespace media {

namespace base {

template<typename Class, typename Type>
SizeBase<Class, Type>::SizeBase(Type width, Type height) {
  set_width(width);
  set_height(height);
}

template<typename Class, typename Type>
SizeBase<Class, Type>::~SizeBase() = default;

template<typename Class, typename Type>
void SizeBase<Class, Type>::set_width(Type width) {
  if (width < 0) {
    NOTREACHED() << "negative width:" << width;
    width = 0;
  }
  width_ = width;
}

template<typename Class, typename Type>
void SizeBase<Class, Type>::set_height(Type height) {
  if (height < 0) {
    NOTREACHED() << "negative height:" << height;
    height = 0;
  }
  height_ = height;
}

Size::Size(int width, int height) : SizeBase(width, height) {}

Size::Size() : SizeBase<Size, int>(0, 0) {
}

std::string Size::ToString() const {
  std::ostringstream stream;
  stream << "Size(" << width() << "," << height() << ")";
  return stream.str();
}

Size::~Size() = default;

}
}