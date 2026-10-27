// Minimal windowed player for Windows without Flutter: GLFW owns the window,
// Direct3D 11 draws the frames the way the Flutter plugin takes them, as
// shared BGRA textures made on the adapter we draw with, or as CPU BGRA
// frames (software decoding, or --cpu) uploaded here. Useful to tell core
// problems from Flutter texture bridge problems.
//
// Keys: space play/pause, left/right seek -/+5 s, escape quit. --key
// SEC:NAME presses one at SEC seconds, --seconds N quits after N seconds.
// --snapshot SEC:FILE writes what the window shows at SEC seconds as a PPM
// for tool/frame_check.py, --size WxH sets the window's client size.

#include <lychee/lychee.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

// Draws a textured quad scaled to fit the back buffer. Texture coordinates
// are rotated clockwise in quarter turns, like Flutter's RotatedBox, then
// scaled to the part of the texture the frame covers. Corners are in
// clockwise order, which D3D11 does not cull.
const char kShaderSource[] = R"(
cbuffer Params : register(b0) {
  float2 scale;
  float2 uv_scale;
  int quarter_turns;
};

Texture2D frame : register(t0);
SamplerState linear_clamp : register(s0);

struct VertexOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};

VertexOut vertex_main(uint id : SV_VertexID) {
  const float2 corners[4] = {float2(-1, 1), float2(1, 1), float2(-1, -1),
                             float2(1, -1)};
  float2 corner = corners[id];
  VertexOut output;
  output.position = float4(corner * scale, 0, 1);
  float2 uv = corner * float2(0.5, -0.5) + 0.5;
  for (int i = 0; i < quarter_turns; ++i) {
    uv = float2(uv.y, 1.0 - uv.x);
  }
  output.uv = uv * uv_scale;
  return output;
}

float4 pixel_main(VertexOut input) : SV_Target {
  return frame.Sample(linear_clamp, input.uv);
}
)";

struct Params {
  float scale[2];
  float uv_scale[2];
  int32_t quarter_turns;
  int32_t padding[3];
};

// The pixels a quad covering |scale| of a |size| pixels wide view fills
// (those whose centers it covers): first, count.
std::pair<int, int> Covered(float scale, int size) {
  const int first = static_cast<int>(std::ceil((1 - scale) / 2 * size - 0.5f));
  const int end = static_cast<int>(std::ceil((1 + scale) / 2 * size - 0.5f));
  return {first, end - first};
}

// Writes BGRA rows as a binary PPM (RGB, top row first).
bool WritePpm(const std::string& path,
              const uint8_t* bgra,
              size_t stride,
              int width,
              int height) {
  FILE* file = fopen(path.c_str(), "wb");
  if (!file) return false;
  fprintf(file, "P6\n%d %d\n255\n", width, height);
  std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
  for (int y = 0; y < height; ++y) {
    const uint8_t* p = bgra + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x, p += 4) {
      row[x * 3] = p[2];
      row[x * 3 + 1] = p[1];
      row[x * 3 + 2] = p[0];
    }
    fwrite(row.data(), 1, row.size(), file);
  }
  fclose(file);
  return true;
}

// The refresh interval of the monitor showing |window|, in microseconds.
int64_t RefreshInterval(HWND window) {
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  // 0 and 1 mean "the hardware's default".
  if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY),
                      &info) &&
      EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency > 1) {
    return 1000000 / mode.dmDisplayFrequency;
  }
  return 1000000 / 60;
}

ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr,
                        nullptr, nullptr, entry, target, 0, 0, &code,
                        &errors))) {
    fprintf(
        stderr, "shader: %s\n",
        errors ? static_cast<const char*>(errors->GetBufferPointer()) : entry);
    return nullptr;
  }
  return code;
}

class Renderer {
 public:
  ~Renderer() {
    if (frame_) lychee_frame_release(frame_);
  }

  // Creates the device on the first adapter, which the player then decodes
  // on and makes its textures for, as the plugin passes Flutter's adapter.
  bool Init(GLFWwindow* window, lychee_player* player, bool cpu) {
    window_ = window;
    player_ = player;
    HWND hwnd = glfwGetWin32Window(window);
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapters1(0, &adapter)) ||
        FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                 nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device_, nullptr, &context_))) {
      fprintf(stderr, "no D3D11 device\n");
      return false;
    }

    // Flip model with a frame latency of one: once the waitable object is
    // signalled, what we draw is shown at the next vblank.
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> swap_chain;
    if (FAILED(factory->CreateSwapChainForHwnd(
            device_.Get(), hwnd, &desc, nullptr, nullptr, &swap_chain)) ||
        FAILED(swap_chain.As(&swap_chain_))) {
      fprintf(stderr, "no swap chain\n");
      return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    swap_chain_->SetMaximumFrameLatency(1);
    waitable_ = swap_chain_->GetFrameLatencyWaitableObject();

    ComPtr<ID3DBlob> vertex = Compile("vertex_main", "vs_5_0");
    ComPtr<ID3DBlob> pixel = Compile("pixel_main", "ps_5_0");
    D3D11_BUFFER_DESC params = {};
    params.ByteWidth = sizeof(Params);
    params.Usage = D3D11_USAGE_DEFAULT;
    params.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW =
        D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (!vertex || !pixel ||
        FAILED(device_->CreateVertexShader(vertex->GetBufferPointer(),
                                           vertex->GetBufferSize(), nullptr,
                                           &vertex_shader_)) ||
        FAILED(device_->CreatePixelShader(pixel->GetBufferPointer(),
                                          pixel->GetBufferSize(), nullptr,
                                          &pixel_shader_)) ||
        FAILED(device_->CreateBuffer(&params, nullptr, &params_)) ||
        FAILED(device_->CreateSamplerState(&sampler, &sampler_))) {
      fprintf(stderr, "no pipeline\n");
      return false;
    }

    if (!cpu) {
      lychee_video_device video_device = {};
      video_device.struct_size = sizeof(video_device);
      video_device.type = LYCHEE_VIDEO_DEVICE_D3D11_ADAPTER;
      video_device.handle = adapter.Get();
      lychee_player_set_video_device(player, &video_device);
    }
    return true;
  }

  // Waits until the swap chain takes the next frame: about once a vsync.
  void Wait() { WaitForSingleObjectEx(waitable_, 100, TRUE); }

  void Draw() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    if (width <= 0 || height <= 0) return;  // Minimized.
    if (width != width_ || height != height_) {
      target_.Reset();
      swap_chain_->ResizeBuffers(
          0, static_cast<UINT>(width), static_cast<UINT>(height),
          DXGI_FORMAT_UNKNOWN,
          DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
      ComPtr<ID3D11Texture2D> back_buffer;
      swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
      device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &target_);
      width_ = width;
      height_ = height;
    }

    const int64_t display_time =
        lychee_now_us() + RefreshInterval(glfwGetWin32Window(window_));
    if (lychee_player_has_new_frame(player_, display_time)) {
      lychee_frame* frame = lychee_player_acquire_frame(player_, display_time);
      if (frame) Show(frame);
    }

    const float black[4] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(target_.Get(), black);
    int32_t video_width = 0, video_height = 0;
    // Where the frame goes, in the back buffer's pixels.
    std::pair<int, int> columns, rows;
    if (view_ &&
        lychee_player_get_video_size(player_, &video_width, &video_height)) {
      Params params = {};
      params.uv_scale[0] = uv_scale_[0];
      params.uv_scale[1] = uv_scale_[1];
      params.quarter_turns = lychee_player_get_video_rotation(player_) / 90;
      if (params.quarter_turns % 2) std::swap(video_width, video_height);
      double video_aspect = static_cast<double>(video_width) / video_height;
      double view_aspect = static_cast<double>(width) / height;
      params.scale[0] =
          static_cast<float>(std::min(1.0, video_aspect / view_aspect));
      params.scale[1] =
          static_cast<float>(std::min(1.0, view_aspect / video_aspect));
      columns = Covered(params.scale[0], width);
      rows = Covered(params.scale[1], height);
      context_->UpdateSubresource(params_.Get(), 0, nullptr, &params, 0, 0);

      const D3D11_VIEWPORT viewport = {
          0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
      context_->RSSetViewports(1, &viewport);
      context_->OMSetRenderTargets(1, target_.GetAddressOf(), nullptr);
      context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
      context_->VSSetConstantBuffers(0, 1, params_.GetAddressOf());
      context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
      context_->PSSetShaderResources(0, 1, view_.GetAddressOf());
      context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
      context_->Draw(4, 0);
      // The flip model discards the back buffer once presented.
      if (!snapshots_.empty()) {
        ReadBack(columns, rows);
      }
    }
    swap_chain_->Present(1, 0);
  }

  // Writes the next drawn frame to |path| once the player has shown one.
  void SnapshotTo(const std::string& path) { snapshots_.push_back(path); }
  const std::vector<std::string>& pending_snapshots() const {
    return snapshots_;
  }

  int64_t d3d11_frames() const { return d3d11_frames_; }
  int64_t cpu_frames() const { return cpu_frames_; }

 private:
  // Reads back what was drawn, through a staging copy of the back buffer.
  void ReadBack(std::pair<int, int> columns, std::pair<int, int> rows) {
    ComPtr<ID3D11Texture2D> back_buffer;
    swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    D3D11_TEXTURE2D_DESC desc = {};
    back_buffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) {
      return;
    }
    context_->CopyResource(staging.Get(), back_buffer.Get());
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return;
    }
    for (const std::string& path : snapshots_) {
      if (!WritePpm(path, static_cast<const uint8_t*>(mapped.pData),
                    mapped.RowPitch, static_cast<int>(desc.Width),
                    static_cast<int>(desc.Height))) {
        printf("snapshot %s failed: cannot write\n", path.c_str());
        continue;
      }
      printf("snapshot %s pts=%" PRId64 " size=%ux%u video=%d,%d,%d,%d\n",
             path.c_str(), pts_, desc.Width, desc.Height, columns.first,
             rows.first, columns.second, rows.second);
    }
    context_->Unmap(staging.Get(), 0);
    fflush(stdout);
    snapshots_.clear();
  }

  void Show(lychee_frame* frame) {
    ComPtr<ID3D11Texture2D> texture;
    switch (lychee_frame_get_type(frame)) {
      case LYCHEE_FRAME_D3D11_TEXTURE: {
        // Made by the player's own device: open it on ours.
        auto* shared = static_cast<ID3D11Texture2D*>(
            lychee_frame_get_native_handle(frame));
        ComPtr<IDXGIResource> resource;
        HANDLE handle = nullptr;
        if (FAILED(shared->QueryInterface(IID_PPV_ARGS(&resource))) ||
            FAILED(resource->GetSharedHandle(&handle)) ||
            FAILED(
                device_->OpenSharedResource(handle, IID_PPV_ARGS(&texture)))) {
          lychee_frame_release(frame);
          return;
        }
        ++d3d11_frames_;
        break;
      }
      case LYCHEE_FRAME_CPU_BGRA:
        texture = Upload(frame);
        if (!texture) {
          lychee_frame_release(frame);
          return;
        }
        ++cpu_frames_;
        break;
      default:
        lychee_frame_release(frame);
        return;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    view_.Reset();
    device_->CreateShaderResourceView(texture.Get(), nullptr, &view_);
    // Textures can be larger than the picture they hold.
    uv_scale_[0] = static_cast<float>(lychee_frame_get_width(frame)) /
                   static_cast<float>(desc.Width);
    uv_scale_[1] = static_cast<float>(lychee_frame_get_height(frame)) /
                   static_cast<float>(desc.Height);
    // The player may reuse a texture once its frame is released, so keep
    // the frame until the next one replaces it. Our draws from it are done
    // by then: the swap chain lets us draw only after the last present.
    if (frame_) lychee_frame_release(frame_);
    frame_ = frame;
    pts_ = lychee_frame_get_pts(frame);
  }

  // Copies a CPU frame into a texture of ours, reused while the size holds.
  ComPtr<ID3D11Texture2D> Upload(lychee_frame* frame) {
    int32_t stride = 0;
    const uint8_t* data = lychee_frame_get_data(frame, &stride);
    const UINT width = static_cast<UINT>(lychee_frame_get_width(frame));
    const UINT height = static_cast<UINT>(lychee_frame_get_height(frame));
    if (!data) return nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    if (upload_) upload_->GetDesc(&desc);
    if (!upload_ || desc.Width != width || desc.Height != height) {
      desc = {};
      desc.Width = width;
      desc.Height = height;
      desc.MipLevels = desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DYNAMIC;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      upload_.Reset();
      if (FAILED(device_->CreateTexture2D(&desc, nullptr, &upload_))) {
        return nullptr;
      }
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(upload_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                             &mapped))) {
      return nullptr;
    }
    for (UINT y = 0; y < height; ++y) {
      memcpy(static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
             data + static_cast<size_t>(y) * stride, width * 4);
    }
    context_->Unmap(upload_.Get(), 0);
    return upload_;
  }

  GLFWwindow* window_ = nullptr;
  lychee_player* player_ = nullptr;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGISwapChain2> swap_chain_;
  HANDLE waitable_ = nullptr;
  ComPtr<ID3D11RenderTargetView> target_;
  int width_ = 0;
  int height_ = 0;
  ComPtr<ID3D11VertexShader> vertex_shader_;
  ComPtr<ID3D11PixelShader> pixel_shader_;
  ComPtr<ID3D11Buffer> params_;
  ComPtr<ID3D11SamplerState> sampler_;
  ComPtr<ID3D11Texture2D> upload_;
  // What is on screen: view_ reads the frame's texture, or upload_.
  lychee_frame* frame_ = nullptr;
  int64_t pts_ = 0;
  std::vector<std::string> snapshots_;
  ComPtr<ID3D11ShaderResourceView> view_;
  float uv_scale_[2] = {1, 1};
  int64_t d3d11_frames_ = 0;
  int64_t cpu_frames_ = 0;
};

void OnKey(GLFWwindow* window, int key, int, int action, int) {
  if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
  auto* player = static_cast<lychee_player*>(glfwGetWindowUserPointer(window));
  switch (key) {
    case GLFW_KEY_SPACE:
      if (lychee_player_is_playing(player)) {
        lychee_player_pause(player);
      } else {
        lychee_player_play(player);
      }
      break;
    case GLFW_KEY_LEFT:
    case GLFW_KEY_RIGHT: {
      int64_t step = key == GLFW_KEY_LEFT ? -5000000 : 5000000;
      int64_t target = lychee_player_get_position(player) + step;
      lychee_player_seek(player, std::max<int64_t>(0, target));
      break;
    }
    case GLFW_KEY_ESCAPE:
      glfwSetWindowShouldClose(window, GLFW_TRUE);
      break;
  }
}

// A key press --key SEC:NAME plays at SEC seconds, for unattended runs.
struct ScriptedKey {
  double at;
  int key;
};

// --snapshot SEC:FILE; the first colon, so FILE may hold one (C:\...).
struct Snapshot {
  double at;
  std::string path;
};

bool ParseSnapshot(const std::string& arg, std::vector<Snapshot>* snapshots) {
  const size_t colon = arg.find(':');
  if (colon == std::string::npos || colon + 1 == arg.size()) return false;
  snapshots->push_back(
      {atof(arg.substr(0, colon).c_str()), arg.substr(colon + 1)});
  return true;
}

bool ParseKey(const std::string& arg, std::vector<ScriptedKey>* keys) {
  const size_t colon = arg.find(':');
  if (colon == std::string::npos) return false;
  const std::string name = arg.substr(colon + 1);
  int key = name == "space"    ? GLFW_KEY_SPACE
            : name == "left"   ? GLFW_KEY_LEFT
            : name == "right"  ? GLFW_KEY_RIGHT
            : name == "escape" ? GLFW_KEY_ESCAPE
                               : GLFW_KEY_UNKNOWN;
  if (key == GLFW_KEY_UNKNOWN) return false;
  keys->push_back({atof(arg.substr(0, colon).c_str()), key});
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string url;
  double seconds = 0;
  bool cpu = false;
  bool hardware = true;
  int window_width = 960, window_height = 540;
  std::vector<ScriptedKey> keys;
  std::vector<Snapshot> snapshots;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--seconds" && i + 1 < argc) {
      seconds = atof(argv[++i]);
    } else if (arg == "--key" && i + 1 < argc && ParseKey(argv[i + 1], &keys)) {
      ++i;
    } else if (arg == "--snapshot" && i + 1 < argc &&
               ParseSnapshot(argv[i + 1], &snapshots)) {
      ++i;
    } else if (arg == "--size" && i + 1 < argc &&
               sscanf(argv[i + 1], "%dx%d", &window_width, &window_height) ==
                   2 &&
               window_width > 0 && window_height > 0) {
      ++i;
    } else if (arg == "--cpu") {
      cpu = true;
    } else if (arg == "--no-hw") {
      hardware = false;
    } else if (!arg.empty() && arg[0] != '-') {
      url = arg;
    } else {
      url.clear();
      break;
    }
  }
  if (url.empty()) {
    fprintf(stderr,
            "usage: %s [--seconds N] [--key SEC:space|left|right|escape]... "
            "[--snapshot SEC:FILE]... [--size WxH] [--cpu] [--no-hw] "
            "<file or url>\n",
            argv[0]);
    return 2;
  }
  std::stable_sort(
      keys.begin(), keys.end(),
      [](const ScriptedKey& a, const ScriptedKey& b) { return a.at < b.at; });
  std::stable_sort(
      snapshots.begin(), snapshots.end(),
      [](const Snapshot& a, const Snapshot& b) { return a.at < b.at; });

  if (!glfwInit()) {
    fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window =
      glfwCreateWindow(window_width, window_height, "lychee", nullptr, nullptr);
  if (!window) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }

  // Shared textures need the video device, which Renderer::Init sets; CPU
  // BGRA frames come from software decoding, and from hardware decoding
  // when --cpu leaves the device out.
  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types =
      cpu ? LYCHEE_FRAME_CPU_BGRA
          : LYCHEE_FRAME_D3D11_TEXTURE | LYCHEE_FRAME_CPU_BGRA;
  config.hardware_decoding = hardware ? 1 : 0;
  lychee_player* player = lychee_player_create(&config);
  glfwSetWindowUserPointer(window, player);
  glfwSetKeyCallback(window, &OnKey);

  int exit_code = 0;
  {
    Renderer renderer;
    if (!renderer.Init(window, player, cpu) ||
        lychee_player_open(player, url.c_str()) != 0) {
      fprintf(stderr, "could not start playback\n");
      exit_code = 1;
    } else {
      lychee_player_play(player);
      const int64_t start = lychee_now_us();
      size_t next_key = 0;
      size_t next_snapshot = 0;
      while (!glfwWindowShouldClose(window)) {
        renderer.Wait();
        glfwPollEvents();
        int32_t state = lychee_player_get_state(player);
        if (state == LYCHEE_STATE_ERROR) {
          char error[512];
          lychee_player_get_error(player, error, sizeof(error));
          fprintf(stderr, "error: %s\n", error);
          exit_code = 1;
          break;
        }
        const double elapsed =
            static_cast<double>(lychee_now_us() - start) / 1e6;
        while (next_key < keys.size() && elapsed >= keys[next_key].at) {
          OnKey(window, keys[next_key++].key, 0, GLFW_PRESS, 0);
        }
        // After the keys due at the same time.
        while (next_snapshot < snapshots.size() &&
               elapsed >= snapshots[next_snapshot].at) {
          renderer.SnapshotTo(snapshots[next_snapshot++].path);
        }
        // Snapshots due at the end get up to 2 s to see a frame drawn.
        if (seconds > 0 && elapsed >= seconds &&
            (renderer.pending_snapshots().empty() || elapsed >= seconds + 2)) {
          break;
        }
        renderer.Draw();
      }
      for (size_t i = next_snapshot; i < snapshots.size(); ++i) {
        renderer.SnapshotTo(snapshots[i].path);
      }
      for (const std::string& path : renderer.pending_snapshots()) {
        printf("snapshot %s failed: no frame drawn by then\n", path.c_str());
      }
    }

    lychee_stats stats = {};
    stats.struct_size = sizeof(stats);
    lychee_player_get_stats(player, &stats);
    printf(
        "summary: decoder=%s hw=%d rendered=%lld dropped=%lld position=%.3f "
        "d3d11=%lld cpu=%lld\n",
        stats.video_decoder, stats.hardware_decoding,
        static_cast<long long>(stats.frames_rendered),
        static_cast<long long>(stats.frames_dropped),
        static_cast<double>(lychee_player_get_position(player)) / 1e6,
        static_cast<long long>(renderer.d3d11_frames()),
        static_cast<long long>(renderer.cpu_frames()));
    // The renderer releases the frame it holds before the player goes.
  }
  lychee_player_destroy(player);
  glfwDestroyWindow(window);
  glfwTerminate();
  return exit_code;
}
