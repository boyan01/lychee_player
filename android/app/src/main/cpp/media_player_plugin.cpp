//
// Created by boyan on 2021/2/16.
//

#include <iostream>
#include <exception>

#include <android/log.h>

#include "android_video_renderer_sink.h"

#include "media_player_plugin.h"

static JavaVM *g_vm;

jint JNI_OnLoad(JavaVM *vm, void * /*reserved*/) {
  g_vm = vm;
  return JNI_VERSION_1_4;
}

void JNI_OnUnload(JavaVM *vm, void * /*reserved*/) {
  g_vm = nullptr;
}

extern "C"
JNIEXPORT void JNICALL
Java_tech_soit_flutter_lychee_MediaPlayerBridge_setupJNI(JNIEnv *env, jclass clazz) {
  __android_log_print(ANDROID_LOG_INFO, "MediaPlayerPlugin", "MediaPlayerBridge_SetupJNI");
  auto bridge_class = reinterpret_cast<jclass>(env->NewGlobalRef(clazz));
  media::AndroidVideoRendererSink::flutter_texture_registry =
      [bridge_class]() -> std::unique_ptr<media::FlutterTextureEntry> {
        JNIEnv *g_env;
        if (g_vm->AttachCurrentThread(&g_env, nullptr) != 0) {
          std::cout << "Failed to attach" << std::endl;
        }
//        int getEnvStat = g_vm->GetEnv(reinterpret_cast<void **>(&g_env), JNI_VERSION_1_6);
//        if (getEnvStat == JNI_EDETACHED) {
//          std::cout << "GetEnv: not attached" << std::endl;
//
//        } else if (getEnvStat == JNI_OK) {
//          //
//        } else if (getEnvStat == JNI_EVERSION) {
//          std::cout << "GetEnv: version not supported" << std::endl;
//        }
        jmethodID register_method_id = g_env->GetStaticMethodID(bridge_class,
                                                                "register",
                                                                "()Ltech/soit/flutter/lychee/FlutterTexture;");
        jobject data = g_env->CallStaticObjectMethod(bridge_class, register_method_id);
        return std::make_unique<media::FlutterTextureEntry>(g_env, g_env->NewGlobalRef(data));
      };
}
