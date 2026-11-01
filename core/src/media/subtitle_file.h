#ifndef LYCHEE_MEDIA_SUBTITLE_FILE_H_
#define LYCHEE_MEDIA_SUBTITLE_FILE_H_

#include <string>
#include <string_view>
#include <vector>

#include "media/subtitle_decoder.h"

namespace lychee {

// The cues of a whole subtitle file (lychee_player_add_subtitle()):
// SubRip, WebVTT or ASS/SSA, told apart by their content rather than the
// file name, as plain text the way TextSubtitleDecoder gives the same
// codecs in a container. A byte order mark says UTF-8 or UTF-16; without
// one the text is in |encoding| (an IANA name, ConvertToUtf8()) or, if
// empty, UTF-8 when it is valid UTF-8 and else the legacy encoding it
// looks like: GB18030, Big5 or windows-1252. Cue times count from the
// file's zero. |format| gets the codec name ("subrip", "webvtt", "ass").
// False if the data is none of them, or has no cue.
bool ParseSubtitleFile(std::string_view data,
                       const std::string& encoding,
                       std::vector<SubtitleCue>* cues,
                       std::string* format);

}  // namespace lychee

#endif  // LYCHEE_MEDIA_SUBTITLE_FILE_H_
