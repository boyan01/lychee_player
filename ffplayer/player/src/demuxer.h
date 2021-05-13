//
// Created by yangbin on 2021/4/5.
//

#ifndef MEDIA_PLAYER_DEMUXER_DEMUXER_H_
#define MEDIA_PLAYER_DEMUXER_DEMUXER_H_

#include "memory"

#include "base/message_loop.h"

#include "demuxer_stream.h"
#include "media_tracks.h"
#include "ffmpeg_glue.h"
#include "blocking_url_protocol.h"

namespace media {

constexpr double kNoTimestamp() {
  return 0;
}

typedef int PipelineStatus;
typedef std::function<void(int)> PipelineStatusCB;

class DemuxerHost {
 public:

  /**
   * Set the duration of media.
   * Duration may be kInfiniteDuration() if the duration is not known.
   */
  virtual void SetDuration(double duration) = 0;

  /**
   * Stops execution of the pipeline due to a fatal error.
   * Do not call this method with PipelineStatus::OK.
   */
  virtual void OnDemuxerError(PipelineStatus error) = 0;

 protected:

  virtual ~DemuxerHost() = default;

};

class Demuxer : public std::enable_shared_from_this<Demuxer> {

 public:

  using MediaTracksUpdatedCB = std::function<void(std::unique_ptr<MediaTracks>)>;

  Demuxer(base::MessageLoop *task_runner,
          std::string url,
          MediaTracksUpdatedCB media_tracks_updated_cb);

  TaskRunner *message_loop() { return task_runner_; }

  void Initialize(DemuxerHost *host, PipelineStatusCB status_cb);

  /**
   * Post a task to perform additional demuxing.
   */
  void PostDemuxTask();

  // Allow DemuxerStream to notify us when there is updated information
  // about what buffered data is available.
  void NotifyBufferingChanged();

  // The pipeline is being stopped either as a result of an error or because
  // the client called Stop().
  virtual void Stop(std::function<void(void)> callback);

  DemuxerStream *GetFirstStream(DemuxerStream::Type type);

  std::vector<DemuxerStream *> GetAllStreams();

  void NotifyCapacityAvailable();

 protected:

  virtual std::string GetDisplayName() const;

 private:

  bool abort_request_;

  void InitializeTask();

  // Carries out demuxing and satisfying stream reads on the demuxer thread.
  void DemuxTask();

  // Signal all DemuxerStream that the stream has ended.
  //
  // Must be called on the demuxer thread.
  void StreamHasEnded();

  // Carries out stopping the demuxer streams on the demuxer thread.
  void StopTask(const std::function<void(void)> &callback);

  // Signal the blocked thread that the read has completed, with |size| bytes
  // read or kReadError in case of error.
  virtual void SignalReadCompleted(int size);

  void OnOpenContextDone(bool open);

  bool StreamsHaveAvailableCapacity();

  DemuxerHost *host_;
  PipelineStatusCB init_callback_;

  TaskRunner *task_runner_;

  std::string url_;

  double duration_ = kNoTimestamp();

  // FFmpeg context handle.
  AVFormatContext *format_context_;

  // |streams_| mirrors the AVStream array in |format_context_|. It contains
  // DemuxerStreams encapsluating AVStream objects at the same index.
  //
  // Since we only support a single audio and video stream, |streams_| will
  // contain NULL entries for additional audio/video streams as well as for
  // stream types that we do not currently support.
  //
  // Once initialized, operations on DemuxerStreams should be carried out
  // on the demuxer thread.
  typedef std::vector<std::shared_ptr<DemuxerStream>> StreamVector;
  StreamVector streams_;

  std::map<MediaTrack::Id, DemuxerStream *> track_id_to_demux_stream_map_;

  // Flag to indicate if read has ever failed. Once set to true, it will
  // never be reset. This flag is set true and accessed in Read().
  bool read_has_failed_;

  bool stopped_ = false;

  int last_read_bytes_;
  int64 read_position_;

  // The first timestamp of the opened media file. This is used to set the
  // starting clock value to match the timestamps in the media file. Default
  // is 0.
  double start_time_;

  MediaTracksUpdatedCB media_tracks_updated_cb_;

  // Whether audio has been disabled for this demuxer (in which case this class
  // drops packets destined for AUDIO demuxer streams on the floor).
  bool audio_disabled_;

  // Set if we know duration of the audio stream. Used when processing end of
  // stream -- at this moment we definitely know duration.
  bool duration_known_;

  DELETE_COPY_AND_ASSIGN(Demuxer);

};

} // namespace media
#endif //MEDIA_PLAYER_DEMUXER_DEMUXER_H_
