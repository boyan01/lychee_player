#include "media/text_subtitle_decoder.h"

#include <cctype>
#include <cstring>

namespace lychee {

namespace {

bool StartsWithTag(std::string_view text, size_t at, const char* name) {
  const size_t length = strlen(name);
  if (at + length > text.size()) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (std::tolower(static_cast<unsigned char>(text[at + i])) != name[i]) {
      return false;
    }
  }
  // The name ends there: <b>, <b >, </b>, <font color=...>.
  const size_t end = at + length;
  return end < text.size() && (text[end] == '>' || text[end] == ' ');
}

// SubRip: the HTML-like tags it has (<b>, <i>, <u>, <s>, <font>) and ASS
// override blocks ({\an8}) go; anything else in angle brackets is text.
std::string SubRipToPlainText(std::string_view text) {
  std::string plain;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '<') {
      size_t name = i + 1 < text.size() && text[i + 1] == '/' ? i + 2 : i + 1;
      size_t close = text.find('>', i);
      if (close != std::string_view::npos &&
          (StartsWithTag(text, name, "b") || StartsWithTag(text, name, "i") ||
           StartsWithTag(text, name, "u") || StartsWithTag(text, name, "s") ||
           StartsWithTag(text, name, "font"))) {
        i = close;
        continue;
      }
    } else if (c == '{' && i + 1 < text.size() && text[i + 1] == '\\') {
      size_t close = text.find('}', i);
      if (close != std::string_view::npos) {
        i = close;
        continue;
      }
    }
    if (c != '\r') {
      plain += c;
    }
  }
  return plain;
}

// WebVTT cue text: every tag (<b>, <c.class>, <v Speaker>, timestamps)
// goes and the character references it has become characters; &nbsp; a
// plain space, as FFmpeg's decoder (an ASS \h) gives it.
std::string WebVttToPlainText(std::string_view text) {
  static const struct {
    const char* reference;
    const char* text;
  } kReferences[] = {
      {"&amp;", "&"},   {"&lt;", "<"},          {"&gt;", ">"},
      {"&lrm;", "\xe2\x80\x8e"}, {"&rlm;", "\xe2\x80\x8f"}, {"&nbsp;", " "},
  };
  std::string plain;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '<') {
      size_t close = text.find('>', i);
      if (close == std::string_view::npos) {
        break;
      }
      i = close;
      continue;
    }
    if (c == '&') {
      bool replaced = false;
      for (const auto& entry : kReferences) {
        const size_t length = strlen(entry.reference);
        if (text.substr(i, length) == entry.reference) {
          plain += entry.text;
          i += length - 1;
          replaced = true;
          break;
        }
      }
      if (replaced) {
        continue;
      }
    }
    if (c != '\r') {
      plain += c;
    }
  }
  return plain;
}

}  // namespace

bool TextSubtitleDecoder::Initialize(const SubtitleConfig& config,
                                     std::string* error) {
  static const char* kCodecs[] = {"subrip", "srt",    "text",    "ass",
                                  "ssa",    "webvtt", "mov_text"};
  for (const char* codec : kCodecs) {
    if (config.codec_name == codec) {
      codec_name_ = config.codec_name;
      return true;
    }
  }
  *error = "no decoder for " + config.codec_name + " subtitles in this build";
  return false;
}

// static
std::string TextSubtitleDecoder::ToPlainText(const std::string& codec_name,
                                             std::string_view packet) {
  std::string text;
  if (codec_name == "ass" || codec_name == "ssa") {
    return AssToPlainText(packet);
  }
  if (codec_name == "mov_text") {
    // A 16-bit length, the text, then style boxes.
    if (packet.size() < 2) {
      return std::string();
    }
    size_t length = (static_cast<uint8_t>(packet[0]) << 8) |
                    static_cast<uint8_t>(packet[1]);
    text = std::string(packet.substr(2, length));
  } else if (codec_name == "webvtt") {
    text = WebVttToPlainText(packet);
  } else {
    text = SubRipToPlainText(packet);
  }
  // Trailing line breaks would add empty lines under the cue.
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ' ||
                           text.back() == '\0')) {
    text.pop_back();
  }
  return text;
}

bool TextSubtitleDecoder::Decode(const EncodedPacket& packet,
                                 std::vector<SubtitleCue>* cues) {
  if (packet.end_of_stream || packet.pts == kNoTimestamp || !packet.buffer) {
    return true;
  }
  SubtitleCue cue;
  cue.start = packet.pts;
  if (packet.duration > 0) {
    cue.end = packet.pts + packet.duration;
  }
  cue.text = ToPlainText(
      codec_name_,
      std::string_view(reinterpret_cast<const char*>(packet.buffer->data()),
                       packet.buffer->size()));
  // An empty cue clears the screen; it is kept so the one before ends.
  cues->push_back(std::move(cue));
  return true;
}

}  // namespace lychee
