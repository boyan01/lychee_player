// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media_tracks.h"

#include <memory>

#include "base/logging.h"

#include "audio_decode_config.h"
#include "video_decode_config.h"

namespace media {

MediaTracks::MediaTracks() = default;

MediaTracks::~MediaTracks() = default;

MediaTrack *MediaTracks::AddAudioTrack(
    const AudioDecodeConfig &config,
    MediaTrack::TrackId bytestream_track_id,
    const MediaTrack::Kind &kind,
    const MediaTrack::Label &label,
    const MediaTrack::Language &language) {
  CHECK(audio_configs_.find(bytestream_track_id) == audio_configs_.end());
  std::unique_ptr<MediaTrack> track = std::make_unique<MediaTrack>(
      MediaTrack::Audio, bytestream_track_id, kind, label, language);
  MediaTrack *track_ptr = track.get();
  tracks_.push_back(std::move(track));
  audio_configs_[bytestream_track_id] = config;
  return track_ptr;
}

MediaTrack *MediaTracks::AddVideoTrack(
    const VideoDecodeConfig &config,
    MediaTrack::TrackId bytestream_track_id,
    const MediaTrack::Kind &kind,
    const MediaTrack::Label &label,
    const MediaTrack::Language &language) {
  CHECK(video_configs_.find(bytestream_track_id) == video_configs_.end());
  std::unique_ptr<MediaTrack> track = std::make_unique<MediaTrack>(
      MediaTrack::Video, bytestream_track_id, kind, label, language);
  MediaTrack *track_ptr = track.get();
  tracks_.push_back(std::move(track));
  video_configs_[bytestream_track_id] = config;
  return track_ptr;
}

const AudioDecodeConfig &MediaTracks::getAudioConfig(
    MediaTrack::TrackId bytestream_track_id) const {
  auto it = audio_configs_.find(bytestream_track_id);
  if (it != audio_configs_.end())
    return it->second;
  static AudioDecodeConfig invalidConfig;
  return invalidConfig;
}

const VideoDecodeConfig &MediaTracks::getVideoConfig(
    MediaTrack::TrackId bytestream_track_id) const {
  auto it = video_configs_.find(bytestream_track_id);
  if (it != video_configs_.end())
    return it->second;
  static VideoDecodeConfig invalidConfig;
  return invalidConfig;
}

}  // namespace media
