//
// Created by boyan on 2021/5/13.
//

#include "null_video_renderer_sink.h"

namespace media {

void NullVideoRendererSink::Start(VideoRendererSink::RenderCallback *callback) {

}
void NullVideoRendererSink::Stop() {

}

int64_t NullVideoRendererSink::Attach() {
  return -1;
}

void NullVideoRendererSink::Detach() {

}

void NullVideoRendererSink::DoRender(std::shared_ptr<VideoFrame> frame) {

}

}