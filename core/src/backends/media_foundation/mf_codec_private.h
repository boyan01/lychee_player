#ifndef LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_CODEC_PRIVATE_H_
#define LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_CODEC_PRIVATE_H_

#include "backends/media_foundation/mf_common.h"
#include "media/stream_info.h"

namespace lychee {

// Keep ASF's complete system media type, including WMV sequence headers
// and WMA's block alignment, byte rate and WAVEFORMATEX extra bytes.
struct MFCodecPrivate final : CodecPrivate {
  Microsoft::WRL::ComPtr<IMFMediaType> type;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_CODEC_PRIVATE_H_
