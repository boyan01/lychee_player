#include "backends/android/aes.h"

#include <mutex>

#include "backends/android/jvm.h"

namespace lychee {
namespace {

struct Crypto {
  jclass cipher = nullptr;
  jclass key = nullptr;
  jclass iv = nullptr;
  jmethodID get_instance = nullptr;
  jmethodID key_init = nullptr;
  jmethodID iv_init = nullptr;
  jmethodID init = nullptr;
  jmethodID finish = nullptr;
};

const Crypto* GetCrypto(JNIEnv* env) {
  static Crypto crypto;
  static bool ok = false;
  static std::once_flag once;
  std::call_once(once, [env] {
    LocalFrame frame(env);
    auto find = [env](const char* name) -> jclass {
      if (env->ExceptionCheck()) {
        return nullptr;
      }
      jclass cls = env->FindClass(name);
      return cls ? static_cast<jclass>(env->NewGlobalRef(cls)) : nullptr;
    };
    crypto.cipher = find("javax/crypto/Cipher");
    crypto.key = find("javax/crypto/spec/SecretKeySpec");
    crypto.iv = find("javax/crypto/spec/IvParameterSpec");
    if (!crypto.cipher || !crypto.key || !crypto.iv) {
      return;
    }
    crypto.get_instance =
        env->GetStaticMethodID(crypto.cipher, "getInstance",
                               "(Ljava/lang/String;)Ljavax/crypto/Cipher;");
    if (!crypto.get_instance) {
      return;
    }
    crypto.key_init =
        env->GetMethodID(crypto.key, "<init>", "([BLjava/lang/String;)V");
    if (!crypto.key_init) {
      return;
    }
    crypto.iv_init = env->GetMethodID(crypto.iv, "<init>", "([B)V");
    if (!crypto.iv_init) {
      return;
    }
    crypto.init = env->GetMethodID(
        crypto.cipher, "init",
        "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V");
    if (!crypto.init) {
      return;
    }
    crypto.finish = env->GetMethodID(crypto.cipher, "doFinal", "([B)[B");
    ok = crypto.finish != nullptr;
  });
  return ok ? &crypto : nullptr;
}

}  // namespace

bool DecryptAes128(const uint8_t* key,
                   const uint8_t* iv,
                   std::vector<uint8_t>* data,
                   std::string* error) {
  JNIEnv* env = AttachedEnv();
  if (!env) {
    *error = "AES-128 HLS needs a Java VM on Android";
    return false;
  }
  LocalFrame frame(env);
  auto failed = [&] {
    *error = "AES-128 decryption failed: " + TakeException(env);
    return false;
  };
  const Crypto* crypto = GetCrypto(env);
  if (!crypto) {
    return failed();
  }
  auto bytes = [env](const uint8_t* source, size_t size) -> jbyteArray {
    auto result = env->NewByteArray(static_cast<jsize>(size));
    if (result) {
      env->SetByteArrayRegion(result, 0, static_cast<jsize>(size),
                              reinterpret_cast<const jbyte*>(source));
    }
    return result;
  };
  jbyteArray key_bytes = bytes(key, 16);
  if (env->ExceptionCheck() || !key_bytes) {
    return failed();
  }
  jbyteArray iv_bytes = bytes(iv, 16);
  if (env->ExceptionCheck() || !iv_bytes) {
    return failed();
  }
  jbyteArray input = bytes(data->data(), data->size());
  if (env->ExceptionCheck() || !input) {
    return failed();
  }
  jstring aes = env->NewStringUTF("AES");
  if (!aes) {
    return failed();
  }
  jobject secret =
      env->NewObject(crypto->key, crypto->key_init, key_bytes, aes);
  if (env->ExceptionCheck() || !secret) {
    return failed();
  }
  jobject parameters = env->NewObject(crypto->iv, crypto->iv_init, iv_bytes);
  if (env->ExceptionCheck() || !parameters) {
    return failed();
  }
  jstring algorithm = env->NewStringUTF("AES/CBC/PKCS5Padding");
  if (!algorithm) {
    return failed();
  }
  jobject cipher = env->CallStaticObjectMethod(crypto->cipher,
                                               crypto->get_instance, algorithm);
  if (env->ExceptionCheck() || !cipher) {
    return failed();
  }
  // Cipher.DECRYPT_MODE = 2; Java's PKCS5Padding also pads AES's 16-byte
  // blocks.
  env->CallVoidMethod(cipher, crypto->init, 2, secret, parameters);
  if (env->ExceptionCheck()) {
    return failed();
  }
  auto output = static_cast<jbyteArray>(
      env->CallObjectMethod(cipher, crypto->finish, input));
  if (env->ExceptionCheck() || !output) {
    return failed();
  }
  data->resize(static_cast<size_t>(env->GetArrayLength(output)));
  env->GetByteArrayRegion(output, 0, static_cast<jsize>(data->size()),
                          reinterpret_cast<jbyte*>(data->data()));
  return !env->ExceptionCheck() || failed();
}

}  // namespace lychee
