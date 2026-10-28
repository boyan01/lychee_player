#include "backends/mp4/mp4_subtitle.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <utility>

#include "backends/hls/hls_playlist.h"
#include "media/subtitle_image.h"
#include "media/text_subtitle_decoder.h"

namespace lychee {
namespace {

constexpr size_t kMaxTextBytes = 1 << 20;
constexpr size_t kMaxNodes = 4096;
constexpr int64_t kMaxTime = int64_t{1} << 60;

bool Space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view Local(std::string_view name) {
  const size_t colon = name.find(':');
  return colon == name.npos ? name : name.substr(colon + 1);
}

void Utf8(uint32_t c, std::string* out) {
  if (c == 0 || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) {
    c = 0xfffd;
  }
  if (c < 0x80) {
    out->push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out->push_back(static_cast<char>(0xc0 | (c >> 6)));
    out->push_back(static_cast<char>(0x80 | (c & 63)));
  } else if (c < 0x10000) {
    out->push_back(static_cast<char>(0xe0 | (c >> 12)));
    out->push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
    out->push_back(static_cast<char>(0x80 | (c & 63)));
  } else {
    out->push_back(static_cast<char>(0xf0 | (c >> 18)));
    out->push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
    out->push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
    out->push_back(static_cast<char>(0x80 | (c & 63)));
  }
}

bool XmlText(std::string_view in, std::string* out) {
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '&') {
      out->push_back(in[i]);
      continue;
    }
    const size_t end = in.find(';', i + 1);
    if (end == in.npos || end - i > 16) {
      return false;
    }
    const std::string_view entity = in.substr(i + 1, end - i - 1);
    if (entity == "amp") {
      *out += '&';
    } else if (entity == "lt") {
      *out += '<';
    } else if (entity == "gt") {
      *out += '>';
    } else if (entity == "quot") {
      *out += '"';
    } else if (entity == "apos") {
      *out += '\'';
    } else if (!entity.empty() && entity[0] == '#') {
      const bool hex = entity.size() > 1 && entity[1] == 'x';
      const std::string digits(entity.substr(hex ? 2 : 1));
      char* tail = nullptr;
      const unsigned long value =
          std::strtoul(digits.c_str(), &tail, hex ? 16 : 10);
      if (digits.empty() || tail != digits.c_str() + digits.size() ||
          value > 0x10ffff) {
        return false;
      }
      Utf8(static_cast<uint32_t>(value), out);
    } else {
      return false;
    }
    i = end;
  }
  return true;
}

bool Number(std::string_view text, double* value) {
  if (text.empty() || text.size() > 40) {
    return false;
  }
  // strtod also accepts NaN, exponents and hexadecimal; TTML does not.
  bool digit = false;
  bool dot = false;
  for (char c : text) {
    if (c >= '0' && c <= '9') {
      digit = true;
    } else if (c == '.' && !dot) {
      dot = true;
    } else {
      return false;
    }
  }
  *value = std::strtod(std::string(text).c_str(), nullptr);
  return digit && std::isfinite(*value);
}

struct Rates {
  double frames = 30;
  double subframes = 1;
  double ticks = 1;
};

// TTML2 10.3.1: clock-time and offset-time. IMSC uses media time.
bool Time(std::string_view text, const Rates& rates, int64_t* out) {
  double seconds = 0;
  if (text.find(':') != text.npos) {
    double fields[4] = {};
    int count = 0;
    size_t begin = 0;
    for (;;) {
      const size_t end = text.find(':', begin);
      if (count == 4 ||
          !Number(text.substr(begin, end - begin), &fields[count])) {
        return false;
      }
      ++count;
      if (end == text.npos) {
        break;
      }
      begin = end + 1;
    }
    if (count < 3 || fields[1] >= 60 || fields[2] >= 60) {
      return false;
    }
    seconds = fields[0] * 3600 + fields[1] * 60 + fields[2];
    if (count == 4) {
      // The fractional frame field is a count of subframes, not a
      // decimal fraction of the frame rate.
      const std::string_view frames = text.substr(text.rfind(':') + 1);
      const size_t dot = frames.find('.');
      double whole = fields[3];
      double sub = 0;
      if (dot != frames.npos && (!Number(frames.substr(0, dot), &whole) ||
                                 !Number(frames.substr(dot + 1), &sub))) {
        return false;
      }
      seconds += (whole + sub / rates.subframes) / rates.frames;
    }
  } else {
    size_t unit = 0;
    while (unit < text.size() &&
           ((text[unit] >= '0' && text[unit] <= '9') || text[unit] == '.')) {
      ++unit;
    }
    double value = 0;
    if (!Number(text.substr(0, unit), &value)) {
      return false;
    }
    const std::string_view suffix = text.substr(unit);
    if (suffix == "h") {
      seconds = value * 3600;
    } else if (suffix == "m") {
      seconds = value * 60;
    } else if (suffix == "s") {
      seconds = value;
    } else if (suffix == "ms") {
      seconds = value / 1000;
    } else if (suffix == "f") {
      seconds = value / rates.frames;
    } else if (suffix == "t") {
      seconds = value / rates.ticks;
    } else {
      return false;
    }
  }
  const double micros = seconds * kMicrosPerSecond;
  if (!std::isfinite(micros) || micros < 0 || micros >= kMaxTime) {
    return false;
  }
  *out = std::llround(micros);
  return true;
}

using Attributes = std::vector<std::pair<std::string_view, std::string_view>>;

std::string_view Attribute(const Attributes& attrs, std::string_view name) {
  for (const auto& attr : attrs) {
    if (Local(attr.first) == name) {
      return attr.second;
    }
  }
  return {};
}

// A start tag's name and quoted attributes; references in timing values
// are not needed by IMSC and are rejected by Time().
bool Tag(std::string_view tag, std::string_view* name, Attributes* attrs) {
  size_t at = 0;
  while (at < tag.size() && !Space(tag[at])) {
    ++at;
  }
  *name = tag.substr(0, at);
  if (name->empty()) {
    return false;
  }
  while (at < tag.size()) {
    while (at < tag.size() && Space(tag[at])) {
      ++at;
    }
    if (at == tag.size()) {
      break;
    }
    const size_t begin = at;
    while (at < tag.size() && !Space(tag[at]) && tag[at] != '=') {
      ++at;
    }
    const std::string_view key = tag.substr(begin, at - begin);
    while (at < tag.size() && Space(tag[at])) {
      ++at;
    }
    if (key.empty() || at == tag.size() || tag[at] != '=') {
      return false;
    }
    ++at;
    while (at < tag.size() && Space(tag[at])) {
      ++at;
    }
    if (at == tag.size() || (tag[at] != '\'' && tag[at] != '"')) {
      return false;
    }
    const char quote = tag[at++];
    const size_t end = tag.find(quote, at);
    if (end == tag.npos || attrs->size() >= 128) {
      return false;
    }
    attrs->emplace_back(key, tag.substr(at, end - at));
    at = end + 1;
  }
  return true;
}

struct Element {
  std::string_view name;
  int64_t start = 0;
  int64_t end = kMaxTime;
  int paragraph = -1;
  bool preserve = false;
  bool hidden = false;
  std::string_view region;
  std::string base;
};
struct Text {
  Element element;
  std::string text;
  bool line_break = false;
};

// IMSC 1.2 sections 10.3/10.4: PNG images exactly fit their pixel-sized
// regions. Origin may use pixels or percentages; no text layout is involved.
struct Region {
  std::string_view origin;
  std::string_view extent;
};

bool Pair(std::string_view text,
          int canvas_width,
          int canvas_height,
          bool percent,
          int* x,
          int* y) {
  while (!text.empty() && Space(text.front()))
    text.remove_prefix(1);
  const size_t split = text.find_first_of(" \t\r\n");
  if (split == text.npos) return false;
  std::string_view values[] = {text.substr(0, split), text.substr(split)};
  const int canvas[] = {canvas_width, canvas_height};
  int result[2] = {};
  for (size_t i = 0; i < 2; ++i) {
    auto value = values[i];
    while (!value.empty() && Space(value.front()))
      value.remove_prefix(1);
    while (!value.empty() && Space(value.back()))
      value.remove_suffix(1);
    const bool relative = percent && value.ends_with('%');
    const size_t suffix = relative ? 1 : 2;
    double number = 0;
    if ((!relative && !value.ends_with("px")) || value.size() <= suffix ||
        !Number(value.substr(0, value.size() - suffix), &number) ||
        (relative && canvas[i] <= 0))
      return false;
    if (relative) number *= canvas[i] / 100.0;
    if (number > 16384) return false;
    result[i] = static_cast<int>(std::lround(number));
  }
  *x = result[0];
  *y = result[1];
  return true;
}

bool Ttml(std::string_view xml,
          int64_t start,
          int64_t end,
          std::vector<SubtitleCue>* cues,
          const SubtitleResourceReader& resources) {
  std::vector<Element> stack(1);
  std::vector<Text> texts;
  Rates rates;
  int paragraphs = 0;
  bool root = false;
  int canvas_width = 0;
  int canvas_height = 0;
  std::map<std::string_view, Region> regions;
  struct Image {
    Element element;
    std::string uri;
  };
  std::vector<Image> images;
  auto add = [&](std::string_view value, bool literal, bool line_break) {
    if (stack.back().paragraph < 0 || stack.back().hidden) {
      return true;
    }
    Text text{stack.back(), {}, line_break};
    if (literal) {
      text.text = value;
    } else if (!XmlText(value, &text.text)) {
      return false;
    }
    texts.push_back(std::move(text));
    return texts.size() <= kMaxNodes;
  };
  for (size_t at = 0; at < xml.size();) {
    if (xml[at] != '<') {
      const size_t next = xml.find('<', at);
      const size_t limit = next == xml.npos ? xml.size() : next;
      if (!add(xml.substr(at, limit - at), false, false)) {
        return false;
      }
      at = limit;
      continue;
    }
    if (xml.substr(at, 4) == "<!--" || xml.substr(at, 9) == "<![CDATA[") {
      const bool cdata = xml.substr(at, 9) == "<![CDATA[";
      const size_t begin = at + (cdata ? 9 : 4);
      const size_t close = xml.find(cdata ? "]]>" : "-->", begin);
      if (close == xml.npos ||
          (cdata && !add(xml.substr(begin, close - begin), true, false))) {
        return false;
      }
      at = close + 3;
      continue;
    }
    if (xml.substr(at, 2) == "<?") {
      const size_t close = xml.find("?>", at + 2);
      if (close == xml.npos) {
        return false;
      }
      at = close + 2;
      continue;
    }
    if (xml.substr(at, 2) == "<!") {
      return false;  // No DTDs or entity expansion.
    }
    size_t close = at + 1;
    char quote = 0;
    for (; close < xml.size(); ++close) {
      const char c = xml[close];
      if (quote != 0) {
        if (c == quote) {
          quote = 0;
        }
      } else if (c == '\'' || c == '"') {
        quote = c;
      } else if (c == '>') {
        break;
      }
    }
    if (close == xml.size()) {
      return false;
    }
    std::string_view tag = xml.substr(at + 1, close - at - 1);
    at = close + 1;
    while (!tag.empty() && Space(tag.back())) {
      tag.remove_suffix(1);
    }
    if (!tag.empty() && tag[0] == '/') {
      tag.remove_prefix(1);
      while (!tag.empty() && Space(tag.back())) {
        tag.remove_suffix(1);
      }
      if (stack.size() <= 1 || tag != stack.back().name) {
        return false;
      }
      stack.pop_back();
      continue;
    }
    const bool empty = !tag.empty() && tag.back() == '/';
    if (empty) {
      tag.remove_suffix(1);
    }
    std::string_view name;
    Attributes attrs;
    if (!Tag(tag, &name, &attrs) || stack.size() > 64) {
      return false;
    }
    if (stack.size() == 1) {
      if (root || Local(name) != "tt") {
        return false;
      }
      root = true;
      double frame_rate = 30;
      const auto frame = Attribute(attrs, "frameRate");
      const auto subframe = Attribute(attrs, "subFrameRate");
      const auto tick = Attribute(attrs, "tickRate");
      if ((!frame.empty() && !Number(frame, &frame_rate)) ||
          (!subframe.empty() && !Number(subframe, &rates.subframes))) {
        return false;
      }
      rates.frames = frame_rate;
      const auto multiplier = Attribute(attrs, "frameRateMultiplier");
      if (!multiplier.empty()) {
        const size_t space = multiplier.find(' ');
        double numerator = 0;
        double denominator = 0;
        if (space == multiplier.npos ||
            !Number(multiplier.substr(0, space), &numerator) ||
            !Number(multiplier.substr(space + 1), &denominator) ||
            denominator <= 0) {
          return false;
        }
        rates.frames *= numerator / denominator;
      }
      rates.ticks = frame.empty() ? 1 : rates.frames * rates.subframes;
      if ((!tick.empty() && !Number(tick, &rates.ticks)) || rates.frames <= 0 ||
          rates.subframes <= 0 || rates.ticks <= 0) {
        return false;
      }
      const auto extent = Attribute(attrs, "extent");
      if (!extent.empty() &&
          (!Pair(extent, 0, 0, false, &canvas_width, &canvas_height) ||
           canvas_width == 0 || canvas_height == 0)) {
        return false;
      }
      const auto time_base = Attribute(attrs, "timeBase");
      if (!time_base.empty() && time_base != "media") {
        return false;
      }
    }
    Element element = stack.back();
    element.name = name;
    const auto begin = Attribute(attrs, "begin");
    const auto finish = Attribute(attrs, "end");
    const auto duration = Attribute(attrs, "dur");
    int64_t value = 0;
    if (!begin.empty()) {
      if (!Time(begin, rates, &value) || value >= kMaxTime - element.start) {
        return false;
      }
      element.start += value;
    }
    if (!finish.empty()) {
      if (!Time(finish, rates, &value) ||
          value >= kMaxTime - stack.back().start) {
        return false;
      }
      element.end = std::min(element.end, stack.back().start + value);
    }
    if (!duration.empty()) {
      if (!Time(duration, rates, &value) || value >= kMaxTime - element.start) {
        return false;
      }
      element.end = std::min(element.end, element.start + value);
    }
    const auto container = Attribute(attrs, "timeContainer");
    if (!container.empty() && container != "par") {
      return false;
    }
    const auto space = Attribute(attrs, "space");
    if (!space.empty()) {
      element.preserve = space == "preserve";
    }
    element.hidden = element.hidden || Local(name) == "head" ||
                     Attribute(attrs, "display") == "none";
    if (Local(name) == "p") {
      element.paragraph = paragraphs++;
      if (paragraphs > static_cast<int>(kMaxNodes)) {
        return false;
      }
    }
    if (!Attribute(attrs, "region").empty()) {
      element.region = Attribute(attrs, "region");
    }
    if (!Attribute(attrs, "base").empty()) {
      std::string base;
      if (!XmlText(Attribute(attrs, "base"), &base)) return false;
      element.base = ResolveUrl(element.base, base);
    }
    if (Local(name) == "region") {
      const auto id = Attribute(attrs, "id");
      if (id.empty() || regions.size() >= kMaxNodes) return false;
      regions[id] = {Attribute(attrs, "origin"), Attribute(attrs, "extent")};
    }
    auto uri = Attribute(attrs, "backgroundImage");
    if (Local(name) == "image") {
      uri = Attribute(attrs, "src");
      const auto type = Attribute(attrs, "type");
      if (uri.empty() || (!type.empty() && type != "image/png")) return false;
    }
    if (!uri.empty() && !element.hidden) {
      if (images.size() >= 64) return false;
      Image image{element, {}};
      if (!XmlText(uri, &image.uri)) return false;
      if (!element.base.empty()) {
        image.uri = ResolveUrl(element.base, image.uri);
      }
      images.push_back(std::move(image));
    }
    stack.push_back(element);
    if (Local(name) == "br" && !add({}, true, true)) {
      return false;
    }
    if (empty) {
      stack.pop_back();
    }
  }
  if (!root || stack.size() != 1) {
    return false;
  }
  size_t image_bytes = 0;
  for (const Image& image : images) {
    const int64_t a = std::max(start, image.element.start);
    const int64_t b = std::min(end, image.element.end);
    if (a >= b) continue;
    auto region = regions.find(image.element.region);
    if (region == regions.end() || !resources) return false;
    std::vector<uint8_t> png;
    int width = 0;
    int height = 0;
    if (!resources(image.uri, &png) || !SubtitlePngSize(png, &width, &height))
      return false;
    const size_t pixels = static_cast<size_t>(width) * height * 4;
    if (image_bytes + pixels > 16 * kMaxTextBytes) return false;
    image_bytes += pixels;
    SubtitleBitmap bitmap;
    int region_width = 0;
    int region_height = 0;
    if (!Pair(region->second.extent, 0, 0, false, &region_width,
              &region_height) ||
        region_width != width || region_height != height ||
        !DecodeSubtitlePng(png, &bitmap))
      return false;
    if (!region->second.origin.empty() &&
        !Pair(region->second.origin, canvas_width, canvas_height, true,
              &bitmap.x, &bitmap.y))
      return false;
    SubtitleCue cue;
    cue.start = a;
    cue.end = b;
    cue.canvas_width = canvas_width;
    cue.canvas_height = canvas_height;
    std::vector<SubtitleBitmap> bitmaps;
    bitmaps.push_back(std::move(bitmap));
    cue.bitmaps =
        std::make_shared<const std::vector<SubtitleBitmap>>(std::move(bitmaps));
    cues->push_back(std::move(cue));
  }
  size_t output_bytes = 0;
  for (int p = 0; p < paragraphs; ++p) {
    std::vector<int64_t> bounds;
    for (const Text& text : texts) {
      if (text.element.paragraph == p) {
        const int64_t a = std::max(start, text.element.start);
        const int64_t b = std::min(end, text.element.end);
        if (a < b) {
          bounds.push_back(a);
          bounds.push_back(b);
        }
      }
    }
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
    for (size_t i = 1; i < bounds.size(); ++i) {
      std::string plain;
      for (const Text& text : texts) {
        if (text.element.paragraph != p || text.element.start > bounds[i - 1] ||
            text.element.end < bounds[i]) {
          continue;
        }
        if (text.line_break) {
          while (!plain.empty() && plain.back() == ' ') {
            plain.pop_back();
          }
          plain += '\n';
        } else {
          for (char c : text.text) {
            if (!text.element.preserve && Space(c)) {
              if (!plain.empty() && plain.back() != ' ' &&
                  plain.back() != '\n') {
                plain += ' ';
              }
            } else {
              plain += c;
            }
          }
        }
      }
      while (!plain.empty() && (plain.back() == ' ' || plain.back() == '\n')) {
        plain.pop_back();
      }
      output_bytes += plain.size();
      if (output_bytes > 4 * kMaxTextBytes || cues->size() >= kMaxNodes) {
        return false;
      }
      if (!plain.empty()) {
        SubtitleCue cue;
        cue.start = bounds[i - 1];
        cue.end = bounds[i];
        cue.text = std::move(plain);
        cues->push_back(std::move(cue));
      }
    }
  }
  return true;
}

uint32_t U32(std::string_view data, size_t at) {
  uint32_t n = 0;
  for (size_t i = at; i < at + 4; ++i) {
    n = (n << 8) | static_cast<uint8_t>(data[i]);
  }
  return n;
}

bool WebVtt(std::string_view sample,
            int64_t start,
            int64_t end,
            std::vector<SubtitleCue>* cues) {
  while (!sample.empty()) {
    if (sample.size() < 8) {
      return false;
    }
    const size_t size = U32(sample, 0);
    if (size < 8 || size > sample.size()) {
      return false;
    }
    if (sample.substr(4, 4) == "vttc") {
      std::string_view box = sample.substr(8, size - 8);
      while (!box.empty()) {
        if (box.size() < 8) {
          return false;
        }
        const size_t child = U32(box, 0);
        if (child < 8 || child > box.size()) {
          return false;
        }
        if (box.substr(4, 4) == "payl") {
          SubtitleCue cue;
          cue.start = start;
          cue.end = end;
          cue.text = TextSubtitleDecoder::ToPlainText("webvtt",
                                                      box.substr(8, child - 8));
          if (cues->size() >= kMaxNodes) {
            return false;
          }
          cues->push_back(std::move(cue));
        }
        box.remove_prefix(child);
      }
    }
    sample.remove_prefix(size);
  }
  return true;
}

}  // namespace

bool ParseMp4Subtitle(std::string_view format,
                      std::string_view sample,
                      int64_t start,
                      int64_t duration,
                      std::vector<SubtitleCue>* cues,
                      const SubtitleResourceReader& resources) {
  if (sample.size() > kMaxTextBytes || start < -kMaxTime || start >= kMaxTime ||
      duration <= 0 || duration >= kMaxTime - start) {
    return false;
  }
  const int64_t end = start + duration;
  std::vector<SubtitleCue> parsed;
  const bool ok =
      format == "wvtt"
          ? WebVtt(sample, start, end, &parsed)
          : format == "stpp" && Ttml(sample, start, end, &parsed, resources);
  if (ok) {
    cues->insert(cues->end(), std::make_move_iterator(parsed.begin()),
                 std::make_move_iterator(parsed.end()));
  }
  return ok;
}

}  // namespace lychee
