//
// Created by boyan on 2021/2/12.
//

#ifndef FFPLAYER_FFP_DEFINE_H
#define FFPLAYER_FFP_DEFINE_H

#include <memory>

template<typename T>
using unique_ptr_d = std::unique_ptr<T, void (*)(T *ptr)>;

#endif //FFPLAYER_FFP_DEFINE_H
