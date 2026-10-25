#include "lychee_texture.h"

#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

namespace lychee_flutter {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int64_t kMicrosPerSecond = 1000000;
// How often the vsync thread checks which monitor shows the window.
constexpr int64_t kMonitorCheckInterval = kMicrosPerSecond;

// The DXGI output that drives |monitor|, or null.
ComPtr<IDXGIOutput> FindOutput(IDXGIFactory1* factory, HMONITOR monitor) {
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    ComPtr<IDXGIOutput> output;
    for (UINT j = 0; adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND;
         ++j) {
      DXGI_OUTPUT_DESC desc;
      if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
        return output;
      }
    }
  }
  return nullptr;
}

// The refresh interval of |monitor| in microseconds.
int64_t RefreshInterval(HMONITOR monitor) {
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  // 0 and 1 mean "the hardware's default".
  if (GetMonitorInfoW(monitor, &info) &&
      EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency > 1) {
    return kMicrosPerSecond / mode.dmDisplayFrequency;
  }
  return kMicrosPerSecond / 60;
}

}  // namespace

LycheeTexture::LycheeTexture(lychee_player* player,
                             flutter::TextureRegistrar* registrar,
                             HWND window)
    : registrar_(registrar),
      window_(window),
      texture_(flutter::PixelBufferTexture(
          [this](size_t width, size_t height) {
            return CopyPixelBuffer(width, height);
          })),
      player_(player) {}

LycheeTexture::~LycheeTexture() {
  Dispose();
}

int64_t LycheeTexture::Start() {
  texture_id_ = registrar_->RegisterTexture(&texture_);
  thread_ = std::thread(&LycheeTexture::VsyncLoop, this);
  return texture_id_;
}

void LycheeTexture::Dispose() {
  stopping_ = true;
  // Waits at most one vblank.
  if (thread_.joinable()) {
    thread_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  player_ = nullptr;
}

// Waits for each vblank of the monitor showing the window and asks the
// player about the one after it: what Flutter draws now is composed then.
// Following the window's monitor matters with monitors at different refresh
// rates, as on macOS.
void LycheeTexture::VsyncLoop() {
  SetThreadDescription(GetCurrentThread(), L"lychee-vsync");
  ComPtr<IDXGIFactory1> factory;
  ComPtr<IDXGIOutput> output;
  HMONITOR monitor = nullptr;
  int64_t interval = kMicrosPerSecond / 60;
  bool checked = false;
  int64_t monitor_checked = 0;
  while (!stopping_) {
    const int64_t now = lychee_now_us();
    if (!checked || now - monitor_checked >= kMonitorCheckInterval) {
      checked = true;
      monitor_checked = now;
      // A stale factory does not list monitors attached since.
      if (!factory || !factory->IsCurrent()) {
        factory.Reset();
        output.Reset();
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
      }
      HMONITOR current = MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY);
      if (factory && (current != monitor || !output)) {
        monitor = current;
        output = FindOutput(factory.Get(), monitor);
        interval = RefreshInterval(monitor);
      }
    }
    if (!output || FAILED(output->WaitForVBlank())) {
      // No output to wait on (e.g. a remote session): pace by the clock.
      output.Reset();
      Sleep(static_cast<DWORD>(interval / 1000));
    }
    const int64_t display_time = lychee_now_us() + interval;
    next_display_time_ = display_time;
    bool has_new_frame;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Called every vsync, drawn or not: the player estimates the
      // display's refresh interval from these calls.
      has_new_frame =
          player_ && lychee_player_has_new_frame(player_, display_time) != 0;
    }
    if (has_new_frame && !frame_pending_.exchange(true)) {
      registrar_->MarkTextureFrameAvailable(texture_id_);
    }
  }
}

// Raster thread.
const FlutterDesktopPixelBuffer* LycheeTexture::CopyPixelBuffer(size_t,
                                                                size_t) {
  frame_pending_ = false;
  const int64_t display_time =
      (std::max)(next_display_time_.load(), lychee_now_us());
  lychee_frame* frame = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (player_) {
      frame = lychee_player_acquire_frame(player_, display_time);
    }
  }
  if (!frame) {
    return nullptr;
  }
  int32_t stride = 0;
  const uint8_t* data = lychee_frame_get_type(frame) == LYCHEE_FRAME_CPU_RGBA
                            ? lychee_frame_get_data(frame, &stride)
                            : nullptr;
  if (!data) {
    lychee_frame_release(frame);
    return nullptr;
  }
  const int32_t width = lychee_frame_get_width(frame);
  const int32_t height = lychee_frame_get_height(frame);
  const size_t row = static_cast<size_t>(width) * 4;
  buffer_.width = static_cast<size_t>(width);
  buffer_.height = static_cast<size_t>(height);
  if (static_cast<size_t>(stride) == row) {
    // Flutter reads the frame's own pixels and releases it after the upload.
    buffer_.buffer = data;
    buffer_.release_context = frame;
    buffer_.release_callback = [](void* context) {
      lychee_frame_release(static_cast<lychee_frame*>(context));
    };
  } else {
    // Flutter wants tightly packed rows.
    pixels_.resize(row * static_cast<size_t>(height));
    for (int32_t y = 0; y < height; ++y) {
      std::memcpy(pixels_.data() + static_cast<size_t>(y) * row,
                  data + static_cast<size_t>(y) * stride, row);
    }
    lychee_frame_release(frame);
    buffer_.buffer = pixels_.data();
    buffer_.release_context = nullptr;
    buffer_.release_callback = nullptr;
  }
  return &buffer_;
}

}  // namespace lychee_flutter
