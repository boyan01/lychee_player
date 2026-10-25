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

// Candidates in priority order: hardware first if allowed. Decoders that
// produce CPU frames produce one of the |accepted_storage| types if they can.
// |output| is what RetainVideoOutput() returned, or null.
std::vector<std::unique_ptr<VideoDecoder>> CreateVideoDecoders(
    bool allow_hardware,
    uint32_t accepted_storage,
    const std::shared_ptr<void>& output);

// Takes a reference to the platform object passed to
// lychee_player_set_video_output(), dropped with the last copy. Null for
// null, and on platforms that take none.
std::shared_ptr<void> RetainVideoOutput(void* output);

std::vector<std::unique_ptr<AudioDecoder>> CreateAudioDecoders();

std::unique_ptr<AudioSink> CreateAudioSink();

std::unique_ptr<FrameConverter> CreateFrameConverter();

// Null if this build has no subtitle decoder.
std::unique_ptr<SubtitleDecoder> CreateSubtitleDecoder();

}  // namespace lychee

#endif  // LYCHEE_MEDIA_FACTORY_H_
