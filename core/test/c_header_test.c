// Bindings (Dart FFI, bindgen, Swift, JNI) read lychee.h as C. Building this
// file as strict C99 with -Wpedantic, in the zero-warning build, keeps the
// header plain C.
#include "lychee/lychee.h"

int32_t lychee_c_header_test(void) {
  lychee_config config = {sizeof(lychee_config)};
  return lychee_api_version() + (int32_t)config.struct_size;
}
