//
// Created by yangbin on 2021/4/2.
//

#ifndef MEDIA_PLAYER_F_FMPEG_GLUE_H_
#define MEDIA_PLAYER_F_FMPEG_GLUE_H_

#include <string>
#include <mutex>
#include <map>

#include <base/basictypes.h>

struct URLProtocol;

namespace media {

class FFmpegUrlProtocol {

 public:
  FFmpegUrlProtocol() = default;

  // Read the given amount of bytes into data, returns the number of bytes read
  // if successful, kReadError otherwise.
  virtual size_t Read(size_t size, uint8 *data) = 0;

  // Returns true and the current file position for this file, false if the
  // file position could not be retrieved.
  virtual bool GetPosition(int64 *position_out) = 0;

  // Returns true if the file position could be set, false otherwise.
  virtual bool SetPosition(int64 position) = 0;

  // Returns true and the file size, false if the file size could not be
  // retrieved.
  virtual bool GetSize(int64 *size_out) = 0;

  // Returns false if this protocol supports random seeking.
  virtual bool IsStreaming() = 0;

 protected:
  virtual ~FFmpegUrlProtocol() = default;

 private:
  DISALLOW_COPY_AND_ASSIGN(FFmpegUrlProtocol);

};

class FFmpegGlue {

 public:

  /**
   * @return the singleton instance.
   */
  static FFmpegGlue *GetInstance();

  static URLProtocol *url_protocol();

  // Adds a FFmpegProtocol to the FFmpeg glue layer and returns a unique string
  // that can be passed to FFmpeg to identify the data source.
  std::string AddProtocol(FFmpegUrlProtocol *protocol);

  // Removes a FFmpegProtocol from the FFmpeg glue layer.  Using strings from
  // previously added FFmpegProtocols will no longer work.
  void RemoveProtocol(FFmpegUrlProtocol *protocol);

  // Assigns the FFmpegProtocol identified with by the given key to
  // |protocol|, or assigns NULL if no such FFmpegProtocol could be found.
  void GetProtocol(const std::string &key, FFmpegUrlProtocol **protocol);

 private:
  FFmpegGlue();
  virtual ~FFmpegGlue();

  // Returns the unique key for this data source, which can be passed to
  // avformat_open_input() as the filename.
  static std::string GetProtocolKey(FFmpegUrlProtocol *protocol);

  // Mutual exclusion while adding/removing items from the map.
  std::mutex mutex_;

  // Map between keys and FFmpegProtocol references.
  typedef std::map<std::string, FFmpegUrlProtocol *> ProtocolMap;
  ProtocolMap protocols_;

  DISALLOW_COPY_AND_ASSIGN(FFmpegGlue);

};

}

#endif //MEDIA_PLAYER_F_FMPEG_GLUE_H_
