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
// Longest wait for an upload to reach the GPU.
constexpr ULONGLONG kUploadTimeoutMs = 100;

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

// What a frame shows: the same pixels come back from acquire_frame() until
// the player has a newer frame.
const void* FrameContents(lychee_frame* frame) {
  if (lychee_frame_get_type(frame) == LYCHEE_FRAME_D3D11_TEXTURE) {
    return lychee_frame_get_native_handle(frame);
  }
  int32_t stride = 0;
  return lychee_frame_get_data(frame, &stride);
}

}  // namespace

LycheeTexture::LycheeTexture(lychee_player* player,
                             flutter::TextureRegistrar* registrar,
                             HWND window,
                             IDXGIAdapter* adapter)
    : registrar_(registrar), window_(window), player_(player) {
  D3D11_QUERY_DESC query = {D3D11_QUERY_EVENT, 0};
  if (!adapter ||
      FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                               nullptr, 0, D3D11_SDK_VERSION, &device_,
                               nullptr, &context_)) ||
      FAILED(device_->CreateQuery(&query, &completion_))) {
    device_.Reset();
    texture_ = std::make_unique<flutter::TextureVariant>(
        flutter::PixelBufferTexture([this](size_t width, size_t height) {
          return CopyPixelBuffer(width, height);
        }));
    return;
  }
  texture_ = std::make_unique<flutter::TextureVariant>(
      flutter::GpuSurfaceTexture(kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
                                 [this](size_t width, size_t height) {
                                   return CopyGpuSurface(width, height);
                                 }));
  // Hardware-decoded frames then come as textures Flutter's device can
  // open. Before lychee_player_open(), which is when it takes effect.
  lychee_player_set_video_output(player, adapter);
}

LycheeTexture::~LycheeTexture() {
  Dispose();
  if (shown_frame_) {
    lychee_frame_release(shown_frame_);
  }
}

int64_t LycheeTexture::Start() {
  texture_id_ = registrar_->RegisterTexture(texture_.get());
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

lychee_frame* LycheeTexture::AcquireFrame() {
  frame_pending_ = false;
  const int64_t display_time =
      (std::max)(next_display_time_.load(), lychee_now_us());
  std::lock_guard<std::mutex> lock(mutex_);
  return player_ ? lychee_player_acquire_frame(player_, display_time)
                 : nullptr;
}

const FlutterDesktopPixelBuffer* LycheeTexture::CopyPixelBuffer(size_t,
                                                                size_t) {
  lychee_frame* frame = AcquireFrame();
  if (!frame) {
    return nullptr;
  }
  int32_t stride = 0;
  const uint8_t* data = lychee_frame_get_type(frame) == LYCHEE_FRAME_CPU_BGRA
                            ? lychee_frame_get_data(frame, &stride)
                            : nullptr;
  if (!data) {
    lychee_frame_release(frame);
    return nullptr;
  }
  // Flutter wants tightly packed RGBA rows. Frames are BGRA, which the GPU
  // surface path needs; this path is only for when it is unavailable.
  const int32_t width = lychee_frame_get_width(frame);
  const int32_t height = lychee_frame_get_height(frame);
  const size_t row = static_cast<size_t>(width) * 4;
  pixels_.resize(row * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    const uint8_t* source = data + static_cast<size_t>(y) * stride;
    uint8_t* dest = pixels_.data() + static_cast<size_t>(y) * row;
    for (size_t x = 0; x < row; x += 4) {
      dest[x] = source[x + 2];
      dest[x + 1] = source[x + 1];
      dest[x + 2] = source[x];
      dest[x + 3] = source[x + 3];
    }
  }
  lychee_frame_release(frame);
  buffer_.width = static_cast<size_t>(width);
  buffer_.height = static_cast<size_t>(height);
  buffer_.buffer = pixels_.data();
  buffer_.release_context = nullptr;
  buffer_.release_callback = nullptr;
  return &buffer_;
}

const FlutterDesktopGpuSurfaceDescriptor* LycheeTexture::CopyGpuSurface(
    size_t,
    size_t) {
  lychee_frame* frame = AcquireFrame();
  if (!frame) {
    return shown_texture_ ? &descriptor_ : nullptr;
  }
  if (shown_frame_ && FrameContents(frame) == FrameContents(shown_frame_)) {
    lychee_frame_release(frame);
    return &descriptor_;
  }
  ComPtr<ID3D11Texture2D> texture;
  switch (lychee_frame_get_type(frame)) {
    case LYCHEE_FRAME_D3D11_TEXTURE:
      texture = static_cast<ID3D11Texture2D*>(
          lychee_frame_get_native_handle(frame));
      break;
    case LYCHEE_FRAME_CPU_BGRA:
      texture = Upload(frame);
      break;
  }
  ComPtr<IDXGIResource> resource;
  HANDLE handle = nullptr;
  if (!texture || FAILED(texture.As(&resource)) ||
      FAILED(resource->GetSharedHandle(&handle)) || !handle) {
    lychee_frame_release(frame);
    return shown_texture_ ? &descriptor_ : nullptr;
  }
  if (shown_frame_) {
    lychee_frame_release(shown_frame_);
  }
  shown_frame_ = frame;
  shown_texture_ = texture;
  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  descriptor_ = {};
  descriptor_.struct_size = sizeof(descriptor_);
  descriptor_.handle = handle;
  descriptor_.width = desc.Width;
  descriptor_.height = desc.Height;
  descriptor_.visible_width = static_cast<size_t>(lychee_frame_get_width(frame));
  descriptor_.visible_height =
      static_cast<size_t>(lychee_frame_get_height(frame));
  descriptor_.format = kFlutterDesktopPixelFormatBGRA8888;
  return &descriptor_;
}

ComPtr<ID3D11Texture2D> LycheeTexture::Upload(lychee_frame* frame) {
  int32_t stride = 0;
  const uint8_t* data = lychee_frame_get_data(frame, &stride);
  if (!data) {
    return nullptr;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = static_cast<UINT>(lychee_frame_get_width(frame));
  desc.Height = static_cast<UINT>(lychee_frame_get_height(frame));
  desc.MipLevels = desc.ArraySize = 1;
  // ANGLE opens shared textures only in its surface format.
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  const D3D11_SUBRESOURCE_DATA pixels = {data, static_cast<UINT>(stride), 0};
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(device_->CreateTexture2D(&desc, &pixels, &texture))) {
    return nullptr;
  }
  // Another device reads it: wait until the copy has reached the GPU.
  context_->End(completion_.Get());
  context_->Flush();
  const ULONGLONG deadline = GetTickCount64() + kUploadTimeoutMs;
  HRESULT status;
  while ((status = context_->GetData(completion_.Get(), nullptr, 0,
                                     D3D11_ASYNC_GETDATA_DONOTFLUSH)) ==
         S_FALSE) {
    if (GetTickCount64() >= deadline) {
      return nullptr;
    }
    std::this_thread::yield();
  }
  return SUCCEEDED(status) ? texture : nullptr;
}

}  // namespace lychee_flutter
