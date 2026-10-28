#define LOG_TAG "jvm"

#include "backends/android/jvm.h"

#include <android/api-level.h>
#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <mutex>

#include "base/log.h"

namespace lychee {

namespace {

std::atomic<JavaVM*> java_vm{nullptr};

// Detaches threads AttachedEnv() attached when they exit: ART aborts on a
// thread that exits attached.
pthread_key_t detach_key;

void Detach(void*) {
  if (JavaVM* vm = java_vm.load()) {
    vm->DetachCurrentThread();
  }
}

}  // namespace

JavaVM* GetJavaVm() {
  static std::once_flag once;
  std::call_once(once, [] {
    pthread_key_create(&detach_key, &Detach);
    if (java_vm.load()) {
      return;
    }
    // Before API 31 libnativehelper's invocation stub aborts without a
    // JniInvocation instance, rather than reporting that no VM exists.
    if (android_get_device_api_level() < 31) {
      return;
    }
    using GetCreatedJavaVms = jint (*)(JavaVM**, jsize, jsize*);
    void* library = dlopen("libnativehelper.so", RTLD_NOW);
    auto get = library ? reinterpret_cast<GetCreatedJavaVms>(
                             dlsym(library, "JNI_GetCreatedJavaVMs"))
                       : nullptr;
    JavaVM* vm = nullptr;
    jsize count = 0;
    if (get && get(&vm, 1, &count) == JNI_OK && count > 0) {
      java_vm = vm;
    }
  });
  return java_vm.load();
}

JNIEnv* AttachedEnv() {
  JavaVM* vm = GetJavaVm();
  if (!vm) {
    return nullptr;
  }
  JNIEnv* env = nullptr;
  jint status = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (status == JNI_OK) {
    return env;
  }
  JavaVMAttachArgs args = {JNI_VERSION_1_6, "lychee", nullptr};
  if (status != JNI_EDETACHED ||
      vm->AttachCurrentThreadAsDaemon(&env, &args) != JNI_OK) {
    LOGE("cannot attach a thread to the Java VM");
    return nullptr;
  }
  pthread_setspecific(detach_key, env);
  return env;
}

std::string TakeException(JNIEnv* env) {
  jthrowable exception = env->ExceptionOccurred();
  if (!exception) {
    return "";
  }
  env->ExceptionClear();
  std::string result = "Java exception";
  {
    LocalFrame frame(env);
    jclass object = env->FindClass("java/lang/Object");
    jmethodID to_string =
        env->GetMethodID(object, "toString", "()Ljava/lang/String;");
    auto text =
        static_cast<jstring>(env->CallObjectMethod(exception, to_string));
    if (env->ExceptionCheck() || !text) {
      env->ExceptionClear();
    } else {
      const char* chars = env->GetStringUTFChars(text, nullptr);
      result = chars ? chars : "";
      env->ReleaseStringUTFChars(text, chars);
    }
  }
  env->DeleteLocalRef(exception);
  return result;
}

}  // namespace lychee

// Java loads liblychee by name (System.loadLibrary("lychee")) before the
// first player, so its VM is known before any thread asks.
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  lychee::java_vm = vm;
  return JNI_VERSION_1_6;
}
