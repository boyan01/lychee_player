#include "media/subtitle_file.h"

#include <cctype>
#include <cstdint>

#include "media/text_subtitle_decoder.h"

namespace lychee {

namespace {

// More is not a subtitle file, or not one worth holding in memory.
constexpr size_t kMaxCues = 100000;

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

// UTF-8 without its byte order mark; UTF-16 after one, converted (a lone
// surrogate becomes U+FFFD).
std::string ToUtf8(std::string_view data) {
  const auto byte = [&data](size_t i) {
    return static_cast<uint8_t>(data[i]);
  };
  if (data.size() >= 3 && byte(0) == 0xef && byte(1) == 0xbb &&
      byte(2) == 0xbf) {
    return std::string(data.substr(3));
  }
  const bool little = data.size() >= 2 && byte(0) == 0xff && byte(1) == 0xfe;
  const bool big = data.size() >= 2 && byte(0) == 0xfe && byte(1) == 0xff;
  if (!little && !big) {
    return std::string(data);
  }
  const auto unit = [&](size_t i) -> uint32_t {
    return little ? byte(i) | (byte(i + 1) << 8) : (byte(i) << 8) | byte(i + 1);
  };
  std::string text;
  for (size_t i = 2; i + 1 < data.size(); i += 2) {
    const uint32_t high = unit(i);
    if (high >= 0xd800 && high < 0xdc00 && i + 3 < data.size()) {
      const uint32_t low = unit(i + 2);
      if (low >= 0xdc00 && low < 0xe000) {
        AppendUtf8(0x10000 + ((high - 0xd800) << 10) + (low - 0xdc00), &text);
        i += 2;
        continue;
      }
    }
    AppendUtf8(high >= 0xd800 && high < 0xe000 ? 0xfffd : high, &text);
  }
  return text;
}

// Lines without their \n, \r\n or \r.
std::vector<std::string_view> SplitLines(std::string_view text) {
  std::vector<std::string_view> lines;
  size_t begin = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n' || text[i] == '\r') {
      lines.push_back(text.substr(begin, i - begin));
      if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
      begin = i + 1;
    }
  }
  if (begin < text.size()) {
    lines.push_back(text.substr(begin));
  }
  return lines;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text[0]))) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

bool StartsWithNoCase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

// A time at |*pos|, skipping spaces before it: up to three fields
// separated by ':' (hours, minutes, seconds; the first may have any
// number of digits) and a fraction after ',' or '.', as SubRip
// (00:00:01,500), WebVTT (00:01.500) and ASS (0:00:01.50) write it.
// Microseconds, or -1. Leaves |*pos| after it.
int64_t ParseTime(std::string_view line, size_t* pos) {
  size_t i = *pos;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  int64_t fields[3] = {0, 0, 0};
  int count = 0;
  for (;;) {
    const size_t start = i;
    int64_t value = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])) &&
           i - start < 10) {
      value = value * 10 + (line[i] - '0');
      ++i;
    }
    if (i == start) {
      return -1;
    }
    fields[count++] = value;
    if (count == 3 || i >= line.size() || line[i] != ':') {
      break;
    }
    ++i;
  }
  if (count < 2) {
    return -1;
  }
  int64_t micros = 0;
  if (i < line.size() && (line[i] == ',' || line[i] == '.')) {
    ++i;
    int64_t scale = 100000;
    const size_t start = i;
    while (i < line.size() &&
           std::isdigit(static_cast<unsigned char>(line[i]))) {
      micros += (line[i] - '0') * scale;
      scale /= 10;
      ++i;
    }
    if (i == start) {
      return -1;
    }
  }
  const int64_t hours = count == 3 ? fields[0] : 0;
  const int64_t minutes = fields[count - 2];
  const int64_t seconds = fields[count - 1];
  if (minutes >= 60 || seconds >= 60) {
    return -1;
  }
  *pos = i;
  return ((hours * 60 + minutes) * 60 + seconds) * 1000000 + micros;
}

void AddCue(int64_t start,
            int64_t end,
            std::string text,
            std::vector<SubtitleCue>* cues) {
  if (start < 0 || end <= start || text.empty() || cues->size() >= kMaxCues) {
    return;
  }
  SubtitleCue cue;
  cue.start = start;
  cue.end = end;
  cue.text = std::move(text);
  cues->push_back(std::move(cue));
}

// SubRip and WebVTT: a timing line ("START --> END", WebVTT's settings
// after it), then the payload up to a blank line. Lines outside cues
// (SubRip's counters, WebVTT's header, identifiers and NOTE blocks) go.
void ParseTimedBlocks(const std::vector<std::string_view>& lines,
                      size_t first,
                      const std::string& codec,
                      std::vector<SubtitleCue>* cues) {
  for (size_t i = first; i < lines.size(); ++i) {
    const std::string_view line = lines[i];
    const size_t arrow = line.find("-->");
    if (arrow == std::string_view::npos) {
      continue;
    }
    size_t pos = 0;
    const int64_t start = ParseTime(line.substr(0, arrow), &pos);
    pos = arrow + 3;
    const int64_t end = ParseTime(line, &pos);
    std::string payload;
    for (; i + 1 < lines.size() && !Trim(lines[i + 1]).empty(); ++i) {
      if (lines[i + 1].find("-->") != std::string_view::npos) {
        break;
      }
      if (!payload.empty()) {
        payload += '\n';
      }
      payload += lines[i + 1];
    }
    AddCue(start, end, TextSubtitleDecoder::ToPlainText(codec, payload),
           cues);
  }
}

// ASS/SSA: the Dialogue lines of [Events], Start and End their second and
// third fields.
void ParseAss(const std::vector<std::string_view>& lines,
              std::vector<SubtitleCue>* cues) {
  for (std::string_view line : lines) {
    line = Trim(line);
    if (!StartsWithNoCase(line, "Dialogue:")) {
      continue;
    }
    size_t pos = line.find(',');
    if (pos == std::string_view::npos) {
      continue;
    }
    ++pos;
    const int64_t start = ParseTime(line, &pos);
    if (pos >= line.size() || line[pos] != ',') {
      continue;
    }
    ++pos;
    const int64_t end = ParseTime(line, &pos);
    AddCue(start, end, TextSubtitleDecoder::ToPlainText("ass", line), cues);
  }
}

// Has an [Script Info] or [Events] section, wherever comments put it.
bool IsAss(const std::vector<std::string_view>& lines) {
  for (std::string_view line : lines) {
    line = Trim(line);
    if (StartsWithNoCase(line, "[Script Info]") ||
        StartsWithNoCase(line, "[Events]")) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool ParseSubtitleFile(std::string_view data,
                       std::vector<SubtitleCue>* cues,
                       std::string* format) {
  cues->clear();
  format->clear();
  const std::string text = ToUtf8(data);
  const std::vector<std::string_view> lines = SplitLines(text);
  size_t first = 0;
  while (first < lines.size() && Trim(lines[first]).empty()) {
    ++first;
  }
  if (first == lines.size()) {
    return false;
  }
  const std::string_view head = lines[first];
  if (head.substr(0, 6) == "WEBVTT" &&
      (head.size() == 6 || head[6] == ' ' || head[6] == '\t')) {
    *format = "webvtt";
    // The header goes up to the first blank line.
    while (first < lines.size() && !Trim(lines[first]).empty()) {
      ++first;
    }
    ParseTimedBlocks(lines, first, *format, cues);
  } else if (IsAss(lines)) {
    *format = "ass";
    ParseAss(lines, cues);
  } else {
    *format = "subrip";
    ParseTimedBlocks(lines, first, *format, cues);
  }
  if (cues->empty()) {
    format->clear();
    return false;
  }
  return true;
}

}  // namespace lychee
