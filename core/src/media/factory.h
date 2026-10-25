#ifndef LYCHEE_MEDIA_FACTORY_H_
#define LYCHEE_MEDIA_FACTORY_H_

#include <memory>
#include <vector>

#include "media/audio_decoder.h"
#include "media/audio_sink.h"
#include "media/demuxer.h"
#include "media/frame_converter.h"
#include "media/subtitle_decoder.h"
#include "media/video_decoder.h"

namespace lychee {

// Creates the implementations compiled into this build. Which backends exist
// is decided at build time (e.g. LYCHEE_WITH_FFMPEG).

std::unique_ptr<Demuxer> CreateDemuxer();

// Candidates in priority order: hardware first if allowed.
std::vector<std::unique_ptr<VideoDecoder>> CreateVideoDecoders(
    bool allow_hardware);

std::vector<std::unique_ptr<AudioDecoder>> CreateAudioDecoders();

std::unique_ptr<AudioSink> CreateAudioSink();

std::unique_ptr<FrameConverter> CreateFrameConverter();

// Null if this build has no subtitle decoder.
std::unique_ptr<SubtitleDecoder> CreateSubtitleDecoder();

}  // namespace lychee

#endif  // LYCHEE_MEDIA_FACTORY_H_
