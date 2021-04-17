//
// Created by yangbin on 2021/4/5.
//

#include "renderer/audio_renderer.h"

#include "base/logging.h"

namespace media {

AudioRenderer::AudioRenderer() = default;

AudioRenderer::~AudioRenderer() = default;

void AudioRenderer::Initialize(DemuxerStream *audio_stream, PipelineStatusCallback init_callback) {
  DCHECK(audio_stream);
  DCHECK_EQ(audio_stream->type(), DemuxerStream::Type::AUDIO);

  stream_ = audio_stream;

  DoInitialize(std::move(init_callback));
}

} // namespace media