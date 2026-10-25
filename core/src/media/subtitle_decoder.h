#ifndef LYCHEE_MEDIA_SUBTITLE_DECODER_H_
#define LYCHEE_MEDIA_SUBTITLE_DECODER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "media/encoded_packet.h"
#include "media/stream_info.h"

namespace lychee {

// A subtitle picture: premultiplied RGBA rows of |width| * 4 bytes, at
// (x, y) on the canvas it belongs to.
struct SubtitleBitmap {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
};

// A timed piece of plain text or, for bitmap subtitles (PGS, DVB, VobSub),
// pictures, on the media timeline.
struct SubtitleCue {
  int64_t start = kNoTimestamp;
  // kNoTimestamp: until the next cue starts.
  int64_t end = kNoTimestamp;
  std::string text;
  // Bitmap subtitles: the canvas the pictures are placed on, the size the
  // stream was authored for (0 if it does not say: the video's).
  int canvas_width = 0;
  int canvas_height = 0;
  std::shared_ptr<const std::vector<SubtitleBitmap>> bitmaps;
};

// Decodes subtitle packets into cues.
class SubtitleDecoder {
 public:
  virtual ~SubtitleDecoder() = default;

  virtual const char* name() const = 0;
  virtual bool Initialize(const SubtitleConfig& config,
                          std::string* error) = 0;
  // Appends the packet's cues to |cues|. Returns false on fatal errors.
  virtual bool Decode(const EncodedPacket& packet,
                      std::vector<SubtitleCue>* cues) = 0;
  virtual void Flush() = 0;
};

// Renders styled subtitles (ASS/SSA, through libass) into pictures, with
// their fonts, positions and animations. Decode() runs on the read thread
// and the rest on any thread; implementations lock.
class SubtitleRasterizer {
 public:
  virtual ~SubtitleRasterizer() = default;

  virtual bool Initialize(const SubtitleConfig& config,
                          const std::vector<SubtitleFont>& fonts,
                          std::string* error) = 0;
  virtual void Decode(const EncodedPacket& packet) = 0;
  // Identifies the events showing at |media_time|: 0 if none, and another
  // value whenever the set changes.
  virtual uint64_t EventsAt(int64_t media_time) = 0;
  // The pictures at |media_time| on a |width| x |height| canvas covering a
  // video of |video_width| x |video_height|. Returns false, leaving
  // |bitmaps| alone, if they are the same as the last call returned.
  virtual bool Render(int64_t media_time,
                      int width,
                      int height,
                      int video_width,
                      int video_height,
                      std::vector<SubtitleBitmap>* bitmaps) = 0;
};

// The plain text of an ASS/SSA event: |line| is a Dialogue line ("Dialogue:
// Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text") or the
// same fields after Start and End, prefixed by ReadOrder, as in Matroska and
// FFmpeg's decoded rects. Override blocks ({...}) are dropped, \N and \n
// become line breaks, \h a space.
inline std::string AssToPlainText(std::string_view line) {
  constexpr std::string_view kDialogue = "Dialogue:";
  int fields_before_text = 8;
  if (line.substr(0, kDialogue.size()) == kDialogue) {
    line.remove_prefix(kDialogue.size());
    fields_before_text = 9;
  }
  for (int i = 0; i < fields_before_text; ++i) {
    size_t comma = line.find(',');
    if (comma == std::string_view::npos) {
      return std::string();
    }
    line.remove_prefix(comma + 1);
  }
  std::string text;
  bool in_override = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_override) {
      in_override = c != '}';
    } else if (c == '{') {
      in_override = true;
    } else if (c == '\\' && i + 1 < line.size() &&
               (line[i + 1] == 'N' || line[i + 1] == 'n')) {
      text += '\n';
      ++i;
    } else if (c == '\\' && i + 1 < line.size() && line[i + 1] == 'h') {
      text += ' ';
      ++i;
    } else if (c != '\r') {
      text += c;
    }
  }
  // Trailing line breaks would add empty lines under the cue.
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

}  // namespace lychee

#endif  // LYCHEE_MEDIA_SUBTITLE_DECODER_H_
