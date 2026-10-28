#ifndef LYCHEE_BACKENDS_ANDROID_JVM_H_
#define LYCHEE_BACKENDS_ANDROID_JVM_H_

#include <jni.h>

#include <string>

namespace lychee {

// The process's Java VM, for what only Java offers (HttpURLConnection,
// with the platform's proxies, CAs and network security config). Kept by
// JNI_OnLoad when Java loads liblychee (the SDK and the Flutter plugin
// do), or else asked of libnativehelper (API 31). Null in programs without
// one, such as lychee_cli run from adb shell.
JavaVM* GetJavaVm();

// The calling thread's JNIEnv, attaching the thread to the VM until it
// exits. Null without a VM.
JNIEnv* AttachedEnv();

// Clears the pending exception, if any, and returns its toString().
std::string TakeException(JNIEnv* env);

// Local references made in its scope are freed at its end: native threads
// never return to Java, which would free them.
class LocalFrame {
 public:
  explicit LocalFrame(JNIEnv* env) : env_(env) { env_->PushLocalFrame(32); }
  ~LocalFrame() { env_->PopLocalFrame(nullptr); }
  LocalFrame(const LocalFrame&) = delete;
  LocalFrame& operator=(const LocalFrame&) = delete;

 private:
  JNIEnv* const env_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_ANDROID_JVM_H_
