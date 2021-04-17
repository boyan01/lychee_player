//
// Created by yangbin on 2021/4/7.
//

#ifndef MEDIA_BASE_LAMBDA_H_
#define MEDIA_BASE_LAMBDA_H_

#include "functional"

namespace media {

#define WEAK_THIS(TYPE) weak_this(std::weak_ptr<TYPE>(shared_from_this()))

}

#endif //MEDIA_BASE_LAMBDA_H_
