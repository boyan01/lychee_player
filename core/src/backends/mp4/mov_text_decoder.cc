#include "backends/mp4/mov_text_decoder.h"

#include <algorithm>

namespace lychee {

namespace {

void AppendUtf8(uint32_t code_point, std::string* out) {
  if (code_point < 0x80) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out->push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point < 0x10000) {
    out->push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    out->push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
}

}  // namespace

bool MovTextDecoder::Initialize(const SubtitleConfig& config, std::string* error) {
  if (config.codec_name != "mov_text") {
    *error = "no decoder for " + config.codec_name;
    return false;
  }
  return true;
}

// static
std::string MovTextDecoder::SampleText(const uint8_t* data, size_t size) {
  if (size < 2) {
    return std::string();
  }
  size_t length = (static_cast<size_t>(data[0]) << 8) | data[1];
  data += 2;
  length = std::min(length, size - 2);
  std::string text;
  if (length >= 2 && data[0] == 0xfe && data[1] == 0xff) {
    // UTF-16BE; a lone surrogate becomes U+FFFD.
    for (size_t i = 2; i + 1 < length; i += 2) {
      uint32_t unit = (static_cast<uint32_t>(data[i]) << 8) | data[i + 1];
      if (unit >= 0xd800 && unit < 0xdc00 && i + 3 < length) {
        uint32_t low = (static_cast<uint32_t>(data[i + 2]) << 8) | data[i + 3];
        if (low >= 0xdc00 && low < 0xe000) {
          AppendUtf8(0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00), &text);
          i += 2;
          continue;
        }
      }
      AppendUtf8(unit >= 0xd800 && unit < 0xe000 ? 0xfffd : unit, &text);
    }
  } else {
    size_t start = length >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf ? 3 : 0;
    text.assign(reinterpret_cast<const char*>(data) + start, length - start);
  }
  // Line breaks as \n (QuickTime text uses \r); no NULs.
  std::string plain;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        continue;
      }
      c = '\n';
    }
    if (c != '\0') {
      plain.push_back(c);
    }
  }
  // As FFmpeg's text reaches the player (AssToPlainText): no trailing
  // line breaks or spaces.
  while (!plain.empty() && (plain.back() == '\n' || plain.back() == ' ')) {
    plain.pop_back();
  }
  return plain;
}

bool MovTextDecoder::Decode(const EncodedPacket& packet,
                            std::vector<SubtitleCue>* cues) {
  if (packet.end_of_stream || packet.pts == kNoTimestamp || !packet.buffer) {
    return true;
  }
  SubtitleCue cue;
  cue.start = packet.pts;
  if (packet.duration > 0) {
    cue.end = packet.pts + packet.duration;
  }
  cue.text = SampleText(packet.buffer->data(), packet.buffer->size());
  // An empty sample clears the screen; keep it as a cue so the previous
  // open-ended one ends here.
  cues->push_back(std::move(cue));
  return true;
}

}  // namespace lychee
