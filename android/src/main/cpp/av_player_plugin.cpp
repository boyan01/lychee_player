//
// Created by boyan on 2021/2/16.
//

#include <iostream>

#include "ffp_flutter_android.h"

#include "av_player_plugin.h"

JavaVM *g_vm;

extern "C"
JNIEXPORT void JNICALL
Java_tech_soit_flutter_player_MediaPlayerBridge_setupJNI(JNIEnv *env, jobject thiz) {

  env->GetJavaVM(&g_vm);

  jclass clazz = env->FindClass("tech/soit/flutter/player/MediaPlayerBridge");
  jmethodID register_method_id = env->GetMethodID(clazz, "register",
                                                  "()Ltech/soit/flutter/player/FlutterTexture;");
  auto *media_player_bridge = env->NewGlobalRef(thiz);

  media::flutter_texture_registry =
      [media_player_bridge, register_method_id]() -> std::unique_ptr<media::FlutterTextureEntry> {
        JNIEnv *g_env;
        int getEnvStat = g_vm->GetEnv(reinterpret_cast<void **>(&g_env), JNI_VERSION_1_6);
        if (getEnvStat == JNI_EDETACHED) {
          std::cout << "GetEnv: not attached" << std::endl;
          if (g_vm->AttachCurrentThread(&g_env, nullptr) != 0) {
            std::cout << "Failed to attach" << std::endl;
          }
        } else if (getEnvStat == JNI_OK) {
          //
        } else if (getEnvStat == JNI_EVERSION) {
          std::cout << "GetEnv: version not supported" << std::endl;
        }
        jobject data = g_env->CallObjectMethod(media_player_bridge, register_method_id);
        return std::make_unique<media::FlutterTextureEntry>(g_env, data);
      };
}
