#ifndef LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_ASF_DEMUXER_H_
#define LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_ASF_DEMUXER_H_

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "backends/media_foundation/mf_common.h"

#include <wmcontainer.h>
#include "media/data_source.h"
#include "media/demuxer.h"

namespace lychee {

// Windows owns every ASF structure: ContentInfo reads the header, Splitter
// assembles samples, and Indexer supplies packet-aligned seek positions.
class MFAsfDemuxer final : public Demuxer {
 public:
  explicit MFAsfDemuxer(std::unique_ptr<DataSource> source = nullptr);
  ~MFAsfDemuxer() override;

  const char* name() const override { return "asf"; }
  bool Open(const std::string& url, std::string* error) override;
  const MediaInfo& media_info() const override { return info_; }
  void SetEnabledStreams(const std::vector<int>& indices) override;
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;
  void Interrupt() override;
  void Abort() override;
  std::string error() const override { return error_; }
  int64_t download_rate() const override;

 private:
  Microsoft::WRL::ComPtr<IMFMediaBuffer> ReadBuffer(int64_t offset,
                                                    DWORD size,
                                                    bool exact);
  bool Fail(const char* operation, HRESULT hr);
  void InitializeIndexer();

  std::shared_ptr<DataSource> source_;
  Microsoft::WRL::ComPtr<IMFASFContentInfo> content_;
  Microsoft::WRL::ComPtr<IMFASFSplitter> splitter_;
  Microsoft::WRL::ComPtr<IMFASFIndexer> indexer_;
  Microsoft::WRL::ComPtr<IMFByteStream> index_stream_;
  MediaInfo info_;
  std::vector<WORD> stream_ids_;
  std::vector<int> enabled_;
  int64_t offset_ = 0;
  int64_t data_start_ = 0;
  int64_t data_end_ = -1;
  bool samples_pending_ = false;
  std::atomic<bool> aborted_{false};
  std::atomic<bool> interrupted_{false};
  std::string error_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_ASF_DEMUXER_H_
