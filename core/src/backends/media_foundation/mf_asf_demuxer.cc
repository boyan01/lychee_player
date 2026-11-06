#define LOG_TAG "asf"

#include "backends/media_foundation/mf_asf_demuxer.h"

#include <objidl.h>
#include <wrl/implements.h>

#include <algorithm>
#include <limits>
#include <mutex>

#include "backends/media_foundation/mf_codec_private.h"
#include "base/log.h"

namespace lychee {
namespace {

using Microsoft::WRL::ComPtr;

// Adapt random-access input for the system indexer. The indexer's byte
// stream has its own cursor, independent of the splitter's read offset.
class SourceStream final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IStream> {
 public:
  explicit SourceStream(std::shared_ptr<DataSource> source)
      : source_(std::move(source)) {}

  HRESULT STDMETHODCALLTYPE Read(void* data, ULONG size, ULONG* read) override {
    std::lock_guard<std::mutex> guard(mutex_);
    ULONG total = 0;
    while (total < size) {
      const int64_t count = source_->Read(
          position_, static_cast<uint8_t*>(data) + total, size - total);
      if (count <= 0) {
        if (read) *read = total;
        return count < 0 ? E_FAIL : S_FALSE;
      }
      total += static_cast<ULONG>(count);
      position_ += count;
    }
    if (read) *read = total;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
    return STG_E_ACCESSDENIED;
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move,
                                 DWORD origin,
                                 ULARGE_INTEGER* result) override {
    std::lock_guard<std::mutex> guard(mutex_);
    int64_t base = 0;
    if (origin == STREAM_SEEK_CUR) {
      base = position_;
    } else if (origin == STREAM_SEEK_END) {
      base = source_->size();
    } else if (origin != STREAM_SEEK_SET) {
      return STG_E_INVALIDFUNCTION;
    }
    if (base < 0 || (move.QuadPart < 0 && move.QuadPart < -base) ||
        (move.QuadPart > 0 && move.QuadPart > INT64_MAX - base)) {
      return STG_E_INVALIDFUNCTION;
    }
    position_ = base + move.QuadPart;
    if (result) {
      result->QuadPart = static_cast<ULONGLONG>(position_);
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
    if (!stat) {
      return E_POINTER;
    }
    *stat = {};
    if (source_->size() < 0) {
      return E_FAIL;
    }
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = static_cast<ULONGLONG>(source_->size());
    stat->grfMode = STGM_READ;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
    return STG_E_ACCESSDENIED;
  }
  HRESULT STDMETHODCALLTYPE CopyTo(IStream*,
                                   ULARGE_INTEGER,
                                   ULARGE_INTEGER*,
                                   ULARGE_INTEGER*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,
                                       ULARGE_INTEGER,
                                       DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,
                                         ULARGE_INTEGER,
                                         DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

 private:
  std::shared_ptr<DataSource> source_;
  int64_t position_ = 0;
  std::mutex mutex_;
};

VideoCodec VideoSubtype(const GUID& subtype) {
  if (subtype == MFVideoFormat_WMV1) return VideoCodec::kWMV1;
  if (subtype == MFVideoFormat_WMV2) return VideoCodec::kWMV2;
  if (subtype == MFVideoFormat_WMV3) return VideoCodec::kWMV3;
  if (subtype == MFVideoFormat_WVC1) return VideoCodec::kVC1;
  return VideoCodec::kUnknown;
}

AudioCodec AudioSubtype(const GUID& subtype) {
  if (subtype == MFAudioFormat_WMAudioV8) return AudioCodec::kWMA2;
  if (subtype == MFAudioFormat_WMAudioV9) return AudioCodec::kWMAPro;
  if (subtype == MFAudioFormat_WMAudio_Lossless)
    return AudioCodec::kWMALossless;
  return AudioCodec::kUnknown;
}

}  // namespace

MFAsfDemuxer::MFAsfDemuxer(std::unique_ptr<DataSource> source)
    : source_(std::move(source)) {}

MFAsfDemuxer::~MFAsfDemuxer() = default;

bool MFAsfDemuxer::Fail(const char* operation, HRESULT hr) {
  error_ = std::string("asf: ") + operation + ": " + HResultString(hr);
  return false;
}

ComPtr<IMFMediaBuffer> MFAsfDemuxer::ReadBuffer(int64_t offset,
                                                DWORD size,
                                                bool exact) {
  ComPtr<IMFMediaBuffer> buffer;
  BYTE* data = nullptr;
  if (FAILED(MFCreateMemoryBuffer(size, &buffer)) ||
      FAILED(buffer->Lock(&data, nullptr, nullptr))) {
    error_ = "asf: cannot allocate input buffer";
    return nullptr;
  }
  DWORD total = 0;
  while (total < size) {
    const int64_t count =
        source_->Read(offset + total, data + total, size - total);
    if (count < 0) {
      buffer->Unlock();
      error_ = "asf: " + source_->error();
      return nullptr;
    }
    if (count == 0) break;
    total += static_cast<DWORD>(count);
    if (!exact) break;
  }
  buffer->Unlock();
  buffer->SetCurrentLength(total);
  if (exact && total != size) {
    error_ = "asf: truncated header";
    return nullptr;
  }
  return buffer;
}

bool MFAsfDemuxer::Open(const std::string& url, std::string* error) {
  const auto open = [&] {
    if (!source_) source_ = OpenDataSource(url, &error_);
    if (!source_ || !StartMediaFoundation()) return false;
    HRESULT hr = MFCreateASFContentInfo(&content_);
    if (FAILED(hr)) return Fail("create content info", hr);
    auto first = ReadBuffer(0, 30, true);
    QWORD header_size = 0;
    if (!first) return false;
    if (FAILED(hr = content_->GetHeaderSize(first.Get(), &header_size))) {
      return Fail("header size", hr);
    }
    // Bound memory before asking the OS to parse an untrusted header.
    if (header_size < 30 || header_size > 16 * 1024 * 1024 ||
        (source_->size() >= 0 &&
         header_size > static_cast<QWORD>(source_->size()))) {
      error_ = "asf: invalid or oversized header";
      return false;
    }
    auto header = ReadBuffer(0, static_cast<DWORD>(header_size), true);
    if (!header) return false;
    if (FAILED(hr = content_->ParseHeader(header.Get(), 0))) {
      return Fail("parse header", hr);
    }
    ComPtr<IMFPresentationDescriptor> presentation;
    if (FAILED(hr = content_->GeneratePresentationDescriptor(&presentation)) ||
        FAILED(hr = MFCreateASFSplitter(&splitter_)) ||
        FAILED(hr = splitter_->Initialize(content_.Get()))) {
      return Fail("initialize splitter", hr);
    }
    UINT64 start = 0;
    UINT64 length = 0;
    if (FAILED(hr = presentation->GetUINT64(MF_PD_ASF_DATA_START_OFFSET,
                                            &start)) ||
        start > INT64_MAX) {
      return Fail("data offset", FAILED(hr) ? hr : E_INVALIDARG);
    }
    data_start_ = static_cast<int64_t>(start);
    if (SUCCEEDED(presentation->GetUINT64(MF_PD_ASF_DATA_LENGTH, &length)) &&
        length <= static_cast<UINT64>(INT64_MAX - data_start_)) {
      data_end_ = data_start_ + static_cast<int64_t>(length);
    }
    if (source_->size() >= 0 &&
        (data_end_ < 0 || data_end_ > source_->size())) {
      data_end_ = source_->size();
    }
    info_.format_name = "asf";
    const UINT64 duration =
        MFGetAttributeUINT64(presentation.Get(), MF_PD_DURATION, 0);
    if (duration > 0 && duration <= INT64_MAX)
      info_.duration = static_cast<int64_t>(duration / 10);
    DWORD count = 0;
    if (FAILED(hr = presentation->GetStreamDescriptorCount(&count))) {
      return Fail("stream count", hr);
    }
    for (DWORD i = 0; i < count; ++i) {
      ComPtr<IMFStreamDescriptor> descriptor;
      ComPtr<IMFMediaTypeHandler> handler;
      ComPtr<IMFMediaType> type;
      BOOL selected = FALSE;
      DWORD id = 0;
      GUID major = {}, subtype = {};
      if (FAILED(presentation->GetStreamDescriptorByIndex(i, &selected,
                                                          &descriptor)) ||
          FAILED(descriptor->GetStreamIdentifier(&id)) || id > MAXWORD ||
          FAILED(descriptor->GetMediaTypeHandler(&handler)) ||
          FAILED(handler->GetCurrentMediaType(&type)) ||
          FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) ||
          FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)))
        continue;
      StreamInfo stream;
      stream.index = static_cast<int>(info_.streams.size());
      stream.duration = info_.duration;
      auto native = std::make_shared<MFCodecPrivate>();
      native->type = type;
      UINT32 extra_size = 0;
      std::vector<uint8_t> extra;
      if (SUCCEEDED(type->GetBlobSize(MF_MT_USER_DATA, &extra_size)) &&
          extra_size <= 1024 * 1024) {
        extra.resize(extra_size);
        type->GetBlob(MF_MT_USER_DATA, extra.data(), extra_size, nullptr);
      }
      if (major == MFMediaType_Video) {
        stream.type = MediaType::kVideo;
        stream.video.codec = VideoSubtype(subtype);
        stream.video.codec_name = VideoCodecName(stream.video.codec);
        UINT32 width = 0, height = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (width > INT_MAX || height > INT_MAX) continue;
        stream.video.width = static_cast<int>(width);
        stream.video.height = static_cast<int>(height);
        stream.video.extradata = std::move(extra);
        stream.video.codec_private = native;
        if (info_.default_video_stream < 0)
          info_.default_video_stream = stream.index;
      } else if (major == MFMediaType_Audio) {
        stream.type = MediaType::kAudio;
        stream.audio.codec = AudioSubtype(subtype);
        stream.audio.codec_name = AudioCodecName(stream.audio.codec);
        stream.audio.sample_rate = static_cast<int>(MFGetAttributeUINT32(
            type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
        stream.audio.channels = static_cast<int>(
            MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
        stream.audio.extradata = std::move(extra);
        stream.audio.codec_private = native;
        if (info_.default_audio_stream < 0)
          info_.default_audio_stream = stream.index;
      } else
        continue;
      stream_ids_.push_back(static_cast<WORD>(id));
      enabled_.push_back(stream.index);
      info_.streams.push_back(std::move(stream));
    }
    if (info_.streams.empty()) {
      error_ = "asf: no audio or video streams";
      return false;
    }
    SetEnabledStreams(enabled_);
    InitializeIndexer();
    // Unindexed ASF can still seek safely by replaying from the start.
    // This keeps packet parsing and keyframe decisions inside Windows.
    info_.seekable = source_->seekable();
    offset_ = data_start_;
    return true;
  };
  if (open()) return true;
  if (error_.empty()) error_ = "asf: cannot open input";
  *error = error_;
  return false;
}

void MFAsfDemuxer::InitializeIndexer() {
  if (!source_->seekable() || source_->size() < 0) return;
  ComPtr<IMFASFIndexer> indexer;
  QWORD index_offset = 0;
  if (FAILED(MFCreateASFIndexer(&indexer)) ||
      FAILED(indexer->Initialize(content_.Get())) ||
      FAILED(indexer->GetIndexPosition(content_.Get(), &index_offset)) ||
      index_offset >= static_cast<QWORD>(source_->size()))
    return;
  auto stream = Microsoft::WRL::Make<SourceStream>(source_);
  ComPtr<IMFByteStream> bytes;
  if (!stream || FAILED(MFCreateMFByteStreamOnStream(stream.Get(), &bytes)) ||
      FAILED(MFCreateASFIndexerByteStream(bytes.Get(), index_offset,
                                          &index_stream_)))
    return;
  IMFByteStream* streams[] = {index_stream_.Get()};
  if (SUCCEEDED(indexer->SetIndexByteStreams(streams, 1))) {
    indexer_ = indexer;
    LOGD("system index available");
  }
}

void MFAsfDemuxer::SetEnabledStreams(const std::vector<int>& indices) {
  enabled_ = indices;
  std::vector<WORD> selected;
  for (int index : indices) {
    if (index >= 0 && static_cast<size_t>(index) < stream_ids_.size()) {
      selected.push_back(stream_ids_[index]);
    }
  }
  if (splitter_) {
    splitter_->SelectStreams(selected.data(),
                             static_cast<WORD>(selected.size()));
  }
}

DemuxerReadResult MFAsfDemuxer::Read(EncodedPacket* packet) {
  for (;;) {
    if (aborted_ || interrupted_) return DemuxerReadResult::kAborted;
    if (samples_pending_) {
      ComPtr<IMFSample> sample;
      DWORD flags = 0;
      WORD stream_id = 0;
      HRESULT hr = splitter_->GetNextSample(&flags, &stream_id, &sample);
      samples_pending_ = (flags & ASF_STATUSFLAGS_INCOMPLETE) != 0;
      if (FAILED(hr)) {
        Fail("read sample", hr);
        return DemuxerReadResult::kError;
      }
      if (sample) {
        const auto found =
            std::find(stream_ids_.begin(), stream_ids_.end(), stream_id);
        if (found == stream_ids_.end()) continue;
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD size = 0;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) ||
            FAILED(buffer->Lock(&data, nullptr, &size))) {
          error_ = "asf: cannot access sample";
          return DemuxerReadResult::kError;
        }
        *packet = {};
        packet->stream_index = static_cast<int>(found - stream_ids_.begin());
        packet->buffer = PacketBuffer::Copy(data, size);
        buffer->Unlock();
        LONGLONG time = 0;
        if (SUCCEEDED(sample->GetSampleTime(&time)))
          packet->pts = packet->dts = time / 10;
        if (SUCCEEDED(sample->GetSampleDuration(&time)))
          packet->duration = time / 10;
        packet->keyframe =
            info_.streams[packet->stream_index].type == MediaType::kAudio ||
            MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint,
                                 FALSE);
        return DemuxerReadResult::kOk;
      }
    }
    if (data_end_ >= 0 && offset_ >= data_end_)
      return DemuxerReadResult::kEndOfStream;
    const DWORD size = static_cast<DWORD>(
        data_end_ < 0 ? 64 * 1024
                      : std::min<int64_t>(64 * 1024, data_end_ - offset_));
    auto buffer = ReadBuffer(offset_, size, false);
    if (!buffer)
      return (aborted_ || interrupted_) ? DemuxerReadResult::kAborted
                                        : DemuxerReadResult::kError;
    DWORD count = 0;
    buffer->GetCurrentLength(&count);
    if (count == 0) {
      if (data_end_ >= 0 && offset_ < data_end_) {
        error_ = "asf: truncated data";
        return DemuxerReadResult::kError;
      }
      return DemuxerReadResult::kEndOfStream;
    }
    offset_ += count;
    const HRESULT hr = splitter_->ParseData(buffer.Get(), 0, count);
    if (FAILED(hr)) {
      Fail("parse data", hr);
      return DemuxerReadResult::kError;
    }
    samples_pending_ = true;
  }
}

bool MFAsfDemuxer::Seek(int64_t time) {
  if (aborted_ || !info_.seekable) return false;
  interrupted_ = false;
  source_->ClearInterrupt();
  int64_t offset = data_start_;
  if (indexer_ && time > 0) {
    const int stream = info_.default_video_stream >= 0
                           ? info_.default_video_stream
                           : info_.default_audio_stream;
    if (stream >= 0) {
      ASF_INDEX_IDENTIFIER identifier = {GUID_NULL, stream_ids_[stream]};
      PROPVARIANT value = {};
      value.vt = VT_I8;
      value.hVal.QuadPart = std::min<int64_t>(time, INT64_MAX / 10) * 10;
      QWORD relative = 0;
      MFTIME actual = 0;
      if (SUCCEEDED(indexer_->GetSeekPositionForValue(&value, &identifier,
                                                      &relative, &actual, 0)) &&
          relative <= static_cast<QWORD>(INT64_MAX - data_start_) &&
          (data_end_ < 0 ||
           relative < static_cast<QWORD>(data_end_ - data_start_))) {
        offset += static_cast<int64_t>(relative);
        LOGD("system index seek to %lld us at byte %lld",
             static_cast<long long>(actual / 10),
             static_cast<long long>(offset));
      }
    }
  }
  if (aborted_ || interrupted_ || FAILED(splitter_->Flush())) return false;
  offset_ = offset;
  samples_pending_ = false;
  return true;
}

void MFAsfDemuxer::Interrupt() {
  interrupted_ = true;
  if (source_) source_->Interrupt();
}

void MFAsfDemuxer::Abort() {
  aborted_ = true;
  if (source_) source_->Interrupt();
}

int64_t MFAsfDemuxer::download_rate() const {
  return source_ ? source_->transfer_rate() : -1;
}

}  // namespace lychee
