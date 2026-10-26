#define LOG_TAG "mf"

#include "backends/media_foundation/mf_common.h"

#include <combaseapi.h>

#include <cstdio>
#include <cstring>

#include "base/log.h"
#include "base/time.h"

namespace lychee {

using Microsoft::WRL::ComPtr;

bool StartMediaFoundation() {
  static const bool started = [] {
    CO_MTA_USAGE_COOKIE cookie;
    HRESULT hr = CoIncrementMTAUsage(&cookie);
    if (FAILED(hr)) {
      LOGE("CoIncrementMTAUsage: %s", HResultString(hr).c_str());
      return false;
    }
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
      LOGE("MFStartup: %s", HResultString(hr).c_str());
      return false;
    }
    return true;
  }();
  return started;
}

ComPtr<IMFSample> CreateMFSample(const uint8_t* data,
                                 size_t size,
                                 int64_t pts,
                                 int64_t duration) {
  ComPtr<IMFSample> sample;
  ComPtr<IMFMediaBuffer> buffer;
  BYTE* bytes = nullptr;
  if (size > MAXDWORD || FAILED(MFCreateSample(&sample)) ||
      FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer)) ||
      FAILED(buffer->Lock(&bytes, nullptr, nullptr))) {
    return nullptr;
  }
  std::memcpy(bytes, data, size);
  buffer->Unlock();
  if (FAILED(buffer->SetCurrentLength(static_cast<DWORD>(size))) ||
      FAILED(sample->AddBuffer(buffer.Get()))) {
    return nullptr;
  }
  // Media Foundation counts in 100 ns.
  if (pts != kNoTimestamp) {
    sample->SetSampleTime(pts * 10);
  }
  if (duration > 0) {
    sample->SetSampleDuration(duration * 10);
  }
  return sample;
}

ComPtr<IMFSample> CreateMFOutputSample(DWORD size) {
  ComPtr<IMFSample> sample;
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(MFCreateSample(&sample)) ||
      FAILED(MFCreateMemoryBuffer(size, &buffer)) ||
      FAILED(sample->AddBuffer(buffer.Get()))) {
    return nullptr;
  }
  return sample;
}

std::string HResultString(HRESULT hr) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(hr));
  return text;
}

}  // namespace lychee
