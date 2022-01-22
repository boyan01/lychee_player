// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media_track.h"

#include <utility>

#include "base/logging.h"

namespace media {

MediaTrack::MediaTrack(Type type, TrackId bytestream_track_id, Kind kind,
                       Label label, Language lang)
    : type_(type),
      bytestream_track_id_(bytestream_track_id),
      kind_(std::move(kind)),
      label_(std::move(label)),
      language_(std::move(lang)) {}

void MediaTrack::set_id(const MediaTrack::Id &id) {
  DCHECK(id_.empty());
  DCHECK(!id.empty());
  id_ = id;
}

std::ostream &operator<<(std::ostream &os, const MediaTrack &track) {
  os << "type_: " << track.type_
     << " bytestream_track_id_: " << track.bytestream_track_id_
     << " id_: " << track.id_ << " kind_: " << track.kind_
     << " label_: " << track.label_ << " language_: " << track.language_;
  return os;
}

MediaTrack::~MediaTrack() = default;

const char *TrackTypeToStr(MediaTrack::Type type) {
  switch (type) {
    case MediaTrack::Audio:
      return "audio";
    case MediaTrack::Text:
      return "text";
    case MediaTrack::Video:
      return "video";
  }
  NOTREACHED();
  return "INVALID";
}

}  // namespace media
