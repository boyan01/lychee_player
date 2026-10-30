#ifndef LYCHEE_MEDIA_AUDIO_DECODER_H_
#define LYCHEE_MEDIA_AUDIO_DECODER_H_

#include <functional>
#include <memory>
#include <string>

#include "media/audio_buffer.h"
#include "media/encoded_packet.h"
#include "media/stream_info.h"

namespace lychee {

struct AudioOutputFormat {
  int sample_rate = 0;
  int channels = 0;
};

class AudioDecoder {
 public:
  using OutputCallback = std::function<void(std::shared_ptr<AudioBuffer>)>;

  virtual ~AudioDecoder() = default;

  virtual const char* name() const = 0;

  // Decoded audio is converted to |output_format| (interleaved float).
  virtual bool Initialize(const AudioConfig& config,
                          const AudioOutputFormat& output_format,
                          std::string* error) = 0;

  // An end-of-stream packet drains the decoder. Returns false on fatal
  // errors.
  virtual bool Decode(const EncodedPacket& packet,
                      const OutputCallback& output) = 0;

  virtual void Flush() = 0;

  // Flush before a loop reads the source from its beginning. Codecs with
  // different initial delay and seek preroll override this distinction.
  virtual void Restart() { Flush(); }
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_AUDIO_DECODER_H_
