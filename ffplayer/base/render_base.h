//
// Created by boyan on 2021/2/14.
//

#ifndef FFP_RENDER_BASE_H
#define FFP_RENDER_BASE_H

extern "C" {
#include "libavutil/frame.h"
};

class BaseRender {

 public:

  virtual void Abort() = 0;

  /**
   * Whether the renderer is able to immediately render media from the current position.
   *
   * @return Whether the renderer is ready to render media.
   */
  virtual bool IsReady() = 0;
};

#endif //FFP_RENDER_BASE_H
