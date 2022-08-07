//
// Created by yangbin on 2021/3/13.
//

#include "render_base.h"

BaseRender::BaseRender() = default;

void BaseRender::NotifyRenderProceed() {
  if (render_callback_) {
    render_callback_();
  }
}

void BaseRender::SetRenderCallback(std::function<void()> render_callback) {
  render_callback_ = std::move(render_callback);
}
