// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_BASE_MEDIA_TRACKS_H_
#define MEDIA_BASE_MEDIA_TRACKS_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/basictypes.h"

#include "decoder/media_track.h"

namespace media {

class AudioDecoderConfig;
class VideoDecoderConfig;

class MediaTracks {
 public:
  using MediaTracksCollection = std::vector<std::unique_ptr<MediaTrack>>;

  MediaTracks();
  ~MediaTracks();

  // Adds a new audio track. The |bytestreamTrackId| must uniquely identify the
  // track within the bytestream.
  MediaTrack *AddAudioTrack(const AudioDecoderConfig &config,
                            MediaTrack::TrackId bytestream_track_id,
                            const MediaTrack::Kind &kind,
                            const MediaTrack::Label &label,
                            const MediaTrack::Language &language);
  // Adds a new video track. The |bytestreamTrackId| must uniquely identify the
  // track within the bytestream.
  MediaTrack *AddVideoTrack(const VideoDecoderConfig &config,
                            MediaTrack::TrackId bytestream_track_id,
                            const MediaTrack::Kind &kind,
                            const MediaTrack::Label &label,
                            const MediaTrack::Language &language);

  const MediaTracksCollection &tracks() const { return tracks_; }

  const AudioDecoderConfig &getAudioConfig(
      MediaTrack::TrackId bytestream_track_id) const;
  const VideoDecoderConfig &getVideoConfig(
      MediaTrack::TrackId bytestream_track_id) const;

 private:
  MediaTracksCollection tracks_;
  std::map<MediaTrack::TrackId, AudioDecoderConfig> audio_configs_;
  std::map<MediaTrack::TrackId, VideoDecoderConfig> video_configs_;

  DISALLOW_COPY_AND_ASSIGN(MediaTracks);
};

}  // namespace media

#endif  // MEDIA_BASE_MEDIA_TRACKS_H_
