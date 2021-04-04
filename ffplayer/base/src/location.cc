//
// Created by boyan on 2021/3/27.
//

#include "base/location.h"

namespace media {

namespace tracked_objects {

Location::Location(const char *function_name,
                   const char *file_name,
                   int line_number,
                   const void *program_counter)
    : function_name_(function_name),
      file_name_(file_name),
      line_number_(line_number),
      program_counter_(program_counter) {
}

Location::Location()
    : function_name_("Unknown"),
      file_name_("Unknown"),
      line_number_(-1),
      program_counter_(nullptr) {
}

std::string Location::ToString() const {
  return std::string(function_name_) + "@" + file_name_ + ":" +
      std::to_string(line_number_);
}

const void *GetProgramCounter() {
#if defined(COMPILER_MSVC)
  return _ReturnAddress();
#elif defined(COMPILER_GCC)
  return __builtin_extract_return_addr(__builtin_return_address(0));
#endif  // COMPILER_GCC
  return nullptr;
}

}
}

