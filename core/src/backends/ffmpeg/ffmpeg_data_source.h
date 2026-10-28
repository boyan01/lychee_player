#ifndef LYCHEE_BACKENDS_FFMPEG_FFMPEG_DATA_SOURCE_H_
#define LYCHEE_BACKENDS_FFMPEG_FFMPEG_DATA_SOURCE_H_

#include <memory>
#include <string>

#include "media/data_source.h"

namespace lychee {

// http(s) through FFmpeg's own protocols (avio), for Linux, which has no
// system http client: what the own demuxers (HLS, MPEG-TS) read there.
// Connects on the first Read(), so errors (no such host, HTTP 404)
// surface there and Interrupt() reaches the connect. Reconnects on drops
// as FFmpegDemuxer's http does.
std::unique_ptr<DataSource> OpenFFmpegHttpDataSource(const std::string& url,
                                                     std::string* error);

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FFMPEG_FFMPEG_DATA_SOURCE_H_
