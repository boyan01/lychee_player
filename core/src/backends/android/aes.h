#ifndef LYCHEE_BACKENDS_ANDROID_AES_H_
#define LYCHEE_BACKENDS_ANDROID_AES_H_

#include <cstdint>
#include <string>
#include <vector>

namespace lychee {

// AES-128 CBC with PKCS7 padding, through javax.crypto. Requires a Java VM.
bool DecryptAes128(const uint8_t* key,
                   const uint8_t* iv,
                   std::vector<uint8_t>* data,
                   std::string* error);

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_ANDROID_AES_H_
