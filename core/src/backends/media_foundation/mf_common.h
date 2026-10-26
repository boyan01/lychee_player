#ifndef LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_COMMON_H_
#define LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_COMMON_H_

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace lychee {

// Starts Media Foundation once per process and keeps a multithreaded COM
// apartment alive, so the threads that use its objects (the control and
// decode threads, which join no apartment themselves) are in it implicitly.
// Neither is undone: both last until the process exits.
bool StartMediaFoundation();

// A sample holding a copy of |data|, with times in microseconds
// (kNoTimestamp and 0 leave them unset). Null on failure.
Microsoft::WRL::ComPtr<IMFSample> CreateMFSample(const uint8_t* data,
                                                 size_t size,
                                                 int64_t pts,
                                                 int64_t duration);

// A sample with a buffer of |size| bytes for an MFT to write output into.
Microsoft::WRL::ComPtr<IMFSample> CreateMFOutputSample(DWORD size);

// "0x8007000e" and the like, for logs.
std::string HResultString(HRESULT hr);

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_COMMON_H_
