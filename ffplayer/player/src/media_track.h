// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_BASE_MEDIA_TRACK_H_
#define MEDIA_BASE_MEDIA_TRACK_H_

#include <string>
#include <ostream>

#include "base/basictypes.h"

namespace media {

class MediaTrack {
 public:
  enum Type { Text, Audio, Video };
  using Id = std::string;
  using Kind = std::string;
  using Label = std::string;
  using Language = std::string;

  typedef int TrackId;

  MediaTrack(Type type,
             TrackId bytestream_track_id,
             Kind kind,
             Label label,
             Language lang);

  ~MediaTrack();

  Type type() const { return type_; }

  TrackId bytestream_track_id() const {
    return bytestream_track_id_;
  }
  const Kind &kind() const { return kind_; }
  const Label &label() const { return label_; }
  const Language &language() const { return language_; }

  Id id() const { return id_; }
  void set_id(const Id &id);

  friend std::ostream &operator<<(std::ostream &os, const MediaTrack &track);

 private:
  Type type_;

  // |bytestream_track_id_| is read from the bytestream and is guaranteed to be
  // unique only within the scope of single bytestream's initialization segment.
  // But we might have multiple bytestreams (MediaSource might have multiple
  // SourceBuffers attached to it, which translates into ChunkDemuxer having
  // multiple SourceBufferStates and multiple bytestreams) or subsequent init
  // segments may redefine the bytestream ids. Thus bytestream track ids are not
  // guaranteed to be unique at the Demuxer and HTMLMediaElement level. So we
  // generate truly unique media track |id_| on the Demuxer level.
  TrackId bytestream_track_id_;
  Id id_;

  // These properties are read from input streams by stream parsers as specified
  // in https://dev.w3.org/html5/html-sourcing-inband-tracks/.
  Kind kind_;
  Label label_;
  Language language_;
};

// Helper for logging.
const char *TrackTypeToStr(MediaTrack::Type type);

}  // namespace media

#endif  // MEDIA_BASE_MEDIA_TRACK_H_
