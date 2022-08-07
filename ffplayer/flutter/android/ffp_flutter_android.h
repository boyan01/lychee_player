//
// Created by boyan on 2021/2/17.
//

#ifndef ANDROID_FFP_FLUTTER_ANDROID_H
#define ANDROID_FFP_FLUTTER_ANDROID_H

#include "../base/render_video_flutter.h"
#include "render_audio_android.h"

#include <android/log.h>
#include <android/native_window.h>
#include "../../ffp_define.h"
#include "jni.h"

#include "audio_render_oboe.h"

extern "C" {
#include "libswscale/swscale.h"
};

namespace media {

class FlutterTextureEntry {
 private:
  JNIEnv* env;

  jobject j_texture_entry_;

  jmethodID release_;
  int64_t id_;

  ANativeWindow* native_window_;

  bool released_ = false;

 public:
  FlutterTextureEntry(JNIEnv* env, jobject texture);

  ~FlutterTextureEntry();

  inline int64_t id() { return id_; }

  ANativeWindow* native_window() const { return native_window_; };

  void Release();
};

class FlutterAndroidVideoRender : public FlutterVideoRender {
 public:
  typedef std::function<std::unique_ptr<FlutterTextureEntry>()> TextureRegistry;
  static TextureRegistry flutter_texture_registry;

 private:
  ANativeWindow_Buffer window_buffer_;
  struct SwsContext* img_convert_ctx_;

  std::unique_ptr<FlutterTextureEntry> texture_;

 public:
  FlutterAndroidVideoRender();

  ~FlutterAndroidVideoRender();

  void RenderPicture(Frame& frame) override;

  int64_t Attach();

  void Detach();
};

}  // namespace media

#endif  // ANDROID_FFP_FLUTTER_ANDROID_H
