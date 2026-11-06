#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "backends/windows/d3d11_video_processor.h"
#include "test.h"

namespace lychee {
namespace {

Microsoft::WRL::ComPtr<ID3D11Device> CreateTestDevice() {
  for (auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
    const UINT flags = driver == D3D_DRIVER_TYPE_HARDWARE
                           ? D3D11_CREATE_DEVICE_VIDEO_SUPPORT
                           : 0;
    if (FAILED(D3D11CreateDevice(nullptr, driver, nullptr, flags, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, &level,
                                 &context)) ||
        level < D3D_FEATURE_LEVEL_10_0)
      continue;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context;
    if (FAILED(device.As(&video)) || FAILED(context.As(&video_context)))
      continue;
    // WARP and virtual adapters can create a D3D device while exposing no
    // NV12 plane views. That is an absent capability, not a pixel mismatch.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 320;
    desc.Height = 240;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) continue;
    D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    view.Format = DXGI_FORMAT_R8_UNORM;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> luma, chroma;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &view, &luma)))
      continue;
    view.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &view, &chroma)))
      continue;
    return device;
  }
  std::printf("SKIP D3D11 conversion: no video device with NV12 plane views\n");
  return nullptr;
}

TEST(D3D11VideoProcessor, Nv12ChromaStaysCenteredVertically) {
  auto device = CreateTestDevice();
  if (!device) return;
  constexpr int kWidth = 320;
  constexpr int kHeight = 240;
  std::vector<uint8_t> bytes(kWidth * kHeight * 3 / 2, 128);
  std::fill_n(bytes.begin(), kWidth * kHeight, uint8_t{126});
  // One chroma row is redder. Chroma centers are half a luma pixel below
  // the first row of each 2x2 block. Luma row 3 therefore receives one
  // quarter of chroma row 2, never three quarters from the row below.
  for (int x = 0; x < kWidth; x += 2) {
    bytes[kWidth * kHeight + 2 * kWidth + x + 1] = 160;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;

  const D3D11_SUBRESOURCE_DATA input = {bytes.data(), kWidth, 0};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &input, &texture)));
  if (!texture) return;
  D3D11VideoProcessor converter;
  auto frame = converter.Convert(texture.Get(), 0, kWidth, kHeight,
                                 DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
  EXPECT_TRUE(frame != nullptr);
  if (!frame) return;
  auto cpu = converter.Download(*frame);
  EXPECT_TRUE(cpu != nullptr);
  if (!cpu) return;
  const uint8_t* pixel = cpu->data + 3 * cpu->stride + 8 * 4;
  // BT.601 limited range: Y=126, U=128, V=128+(160-128)/4.
  EXPECT_NEAR(pixel[0], 128, 2);
  EXPECT_NEAR(pixel[1], 122, 2);
  EXPECT_NEAR(pixel[2], 141, 2);
}

TEST(D3D11VideoProcessor, Nv12ArraySliceUsesMatrixAndRange) {
  auto device = CreateTestDevice();
  if (!device) return;
  constexpr int kWidth = 320;
  constexpr int kHeight = 240;
  std::vector<uint8_t> neutral(kWidth * kHeight * 3 / 2, 128);
  std::vector<uint8_t> color(kWidth * kHeight * 3 / 2, 126);
  for (size_t i = kWidth * kHeight; i < color.size(); i += 2) {
    color[i] = 112;
    color[i + 1] = 160;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.MipLevels = 1;
  desc.ArraySize = 2;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  const D3D11_SUBRESOURCE_DATA input[] = {{neutral.data(), kWidth, 0},
                                          {color.data(), kWidth, 0}};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, input, &texture)));
  if (!texture) return;
  struct Case {
    DXGI_COLOR_SPACE_TYPE space;
    int red, green, blue;
  };
  const Case cases[] = {
      {DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601, 179, 108, 96},
      {DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, 185, 114, 94},
      {DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601, 171, 109, 98},
      {DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709, 176, 114, 96},
  };
  D3D11VideoProcessor converter;
  for (const auto& c : cases) {
    auto frame =
        converter.Convert(texture.Get(), 1, kWidth - 16, kHeight - 16, c.space);
    EXPECT_TRUE(frame != nullptr);
    if (!frame) return;
    auto cpu = converter.Download(*frame);
    EXPECT_TRUE(cpu != nullptr);
    if (!cpu) return;
    EXPECT_EQ(cpu->width, kWidth - 16);
    EXPECT_EQ(cpu->height, kHeight - 16);
    EXPECT_NEAR(cpu->data[0], c.blue, 2);
    EXPECT_NEAR(cpu->data[1], c.green, 2);
    EXPECT_NEAR(cpu->data[2], c.red, 2);
  }
}

TEST(D3D11VideoProcessor, Nv12ChromaDoesNotReadCodedPadding) {
  auto device = CreateTestDevice();
  if (!device) return;
  constexpr int kCodedWidth = 384;
  constexpr int kCodedHeight = 256;
  constexpr int kWidth = 320;
  constexpr int kHeight = 240;
  std::vector<uint8_t> bytes(kCodedWidth * kCodedHeight * 3 / 2, 126);
  for (int y = 0; y < kCodedHeight / 2; ++y) {
    for (int x = 0; x < kCodedWidth / 2; ++x) {
      const size_t at = kCodedWidth * kCodedHeight + y * kCodedWidth + x * 2;
      const bool padding = x >= kWidth / 2 || y >= kHeight / 2;
      bytes[at] = padding ? 16 : 128;
      bytes[at + 1] = padding ? 240 : 128;
    }
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = kCodedWidth;
  desc.Height = kCodedHeight;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  const D3D11_SUBRESOURCE_DATA input = {bytes.data(), kCodedWidth, 0};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &input, &texture)));
  if (!texture) return;
  D3D11VideoProcessor converter;
  auto frame = converter.Convert(texture.Get(), 0, kWidth, kHeight,
                                 DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
  EXPECT_TRUE(frame != nullptr);
  if (!frame) return;
  auto cpu = converter.Download(*frame);
  EXPECT_TRUE(cpu != nullptr);
  if (!cpu) return;
  // Visible chroma is neutral. Poisoned coded padding must never leak
  // through bilinear reconstruction at the right edge or bottom edge.
  for (const auto& point :
       {std::pair{319, 100}, std::pair{160, 239}, std::pair{319, 239}}) {
    const uint8_t* pixel =
        cpu->data + point.second * cpu->stride + point.first * 4;
    EXPECT_NEAR(pixel[0], 128, 2);
    EXPECT_NEAR(pixel[1], 128, 2);
    EXPECT_NEAR(pixel[2], 128, 2);
  }
}

}  // namespace
}  // namespace lychee
#endif
