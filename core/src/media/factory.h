#ifndef LYCHEE_MEDIA_FACTORY_H_
#define LYCHEE_MEDIA_FACTORY_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "media/audio_decoder.h"
#include "media/audio_sink.h"
#include "media/demuxer.h"
#include "media/frame_converter.h"
#include "media/subtitle_decoder.h"
#include "media/video_decoder.h"
#include "media/video_device.h"

namespace lychee {

// Creates the implementations compiled into this build, or registered by
// Android's optional FFmpeg extension before the first player is created.

std::unique_ptr<Demuxer> CreateDemuxer();

// Candidates in priority order: hardware first if allowed. Decoders that
// produce CPU frames produce one of the |accepted_storage| types if they can.
// |surface| is what RetainVideoSurface() returned, or null.
// |rotate_surface_frames|: decoders rendering into |surface| rotate the
// video upright (lychee_config.rotate_surface_frames).
std::vector<std::unique_ptr<VideoDecoder>> CreateVideoDecoders(
    bool allow_hardware,
    uint32_t accepted_storage,
    const VideoDevice& device,
    const std::shared_ptr<void>& surface,
    bool rotate_surface_frames);

// The lychee_video_device fields as a VideoDevice, retaining the handle.
// False for a type this build does not support.
bool MakeVideoDevice(int32_t type,
                     void* handle,
                     const char* path,
                     VideoDevice* device);

// Takes a reference to the window passed to
// lychee_player_set_video_surface(), dropped with the last copy. Null for
// null, and on platforms that take none.
std::shared_ptr<void> RetainVideoSurface(void* surface);

std::vector<std::unique_ptr<AudioDecoder>> CreateAudioDecoders();

std::unique_ptr<AudioSink> CreateAudioSink();

// Chooses the silent sink without changing the clock. Call before creating
// players; the caller then drives NullAudioSink::RenderAll().
void UseNullAudioSinkForTesting();

std::unique_ptr<FrameConverter> CreateFrameConverter();

// Null if this build has no subtitle decoder.
std::unique_ptr<SubtitleDecoder> CreateSubtitleDecoder();

}  // namespace lychee

#endif  // LYCHEE_MEDIA_FACTORY_H_
