#include "media/subtitle_image.h"

#include <wincodec.h>
#include <wrl/client.h>

namespace lychee {
bool DecodeSubtitlePng(std::span<const uint8_t> bytes, SubtitleBitmap* image) {
  int width = 0;
  int height = 0;
  if (!SubtitlePngSize(bytes, &width, &height)) {
    return false;
  }
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> converter;
  SubtitleBitmap bitmap;
  bitmap.width = width;
  bitmap.height = height;
  bitmap.rgba.resize(static_cast<size_t>(width) * height * 4);
  const bool ok =
      SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&factory))) &&
      SUCCEEDED(factory->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromMemory(
          const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) &&
      SUCCEEDED(factory->CreateDecoderFromStream(
          stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
      SUCCEEDED(decoder->GetFrame(0, &frame)) &&
      SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
      SUCCEEDED(converter->Initialize(
          frame.Get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone,
          nullptr, 0, WICBitmapPaletteTypeCustom)) &&
      SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(width) * 4,
                                      static_cast<UINT>(bitmap.rgba.size()),
                                      bitmap.rgba.data()));
  converter.Reset();
  frame.Reset();
  decoder.Reset();
  stream.Reset();
  factory.Reset();
  if (SUCCEEDED(initialized)) {
    CoUninitialize();
  }
  if (!ok) {
    return false;
  }
  *image = std::move(bitmap);
  return true;
}
}  // namespace lychee
