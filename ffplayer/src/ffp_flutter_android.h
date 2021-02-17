//
// Created by boyan on 2021/2/17.
//

#ifndef ANDROID_FFP_FLUTTER_ANDROID_H
#define ANDROID_FFP_FLUTTER_ANDROID_H

#ifdef _FLUTTER_ANDROID

#include "render_video_flutter.h"
#include <android/log.h>

class FlutterAndroidVideoRender : public FlutterVideoRender {

 public:
  void RenderPicture(Frame &frame) override;

};

#endif //_FLUTTER_ANDROID

#endif //ANDROID_FFP_FLUTTER_ANDROID_H
