#include "media/subtitle_image.h"

#include <Accelerate/Accelerate.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

namespace lychee {
bool DecodeSubtitlePng(std::span<const uint8_t> bytes, SubtitleBitmap* image) {
  int width = 0;
  int height = 0;
  if (!SubtitlePngSize(bytes, &width, &height)) {
    return false;
  }
  CFDataRef data = CFDataCreateWithBytesNoCopy(
      nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()),
      kCFAllocatorNull);
  if (!data) return false;
  CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
  CFRelease(data);
  if (!source) {
    return false;
  }
  CGImageRef decoded = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  CFRelease(source);
  if (!decoded) {
    return false;
  }
  const size_t stride = CGImageGetBytesPerRow(decoded);
  const size_t bits = CGImageGetBitsPerPixel(decoded);
  if (CGImageGetWidth(decoded) != static_cast<size_t>(width) ||
      CGImageGetHeight(decoded) != static_cast<size_t>(height) || bits == 0 ||
      bits > 128 || stride < (static_cast<size_t>(width) * bits + 7) / 8 ||
      stride > kMaxSubtitleImagePixels * 16 / static_cast<size_t>(height)) {
    CGImageRelease(decoded);
    return false;
  }
  CFDataRef pixels = CGDataProviderCopyData(CGImageGetDataProvider(decoded));
  if (!pixels || CFDataGetLength(pixels) < 0 ||
      static_cast<size_t>(CFDataGetLength(pixels)) < stride * height) {
    if (pixels) CFRelease(pixels);
    CGImageRelease(decoded);
    return false;
  }
  SubtitleBitmap bitmap;
  bitmap.width = width;
  bitmap.height = height;
  bitmap.rgba.resize(static_cast<size_t>(width) * height * 4);
  CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  vImage_CGImageFormat format = {};
  format.bitsPerComponent = 8;
  format.bitsPerPixel = 32;
  format.colorSpace = color;
  format.bitmapInfo =
      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
      static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big);
  vImage_Buffer buffer = {
      bitmap.rgba.data(), static_cast<vImagePixelCount>(height),
      static_cast<vImagePixelCount>(width), static_cast<size_t>(width) * 4};
  vImage_CGImageFormat input = {};
  input.bitsPerComponent =
      static_cast<uint32_t>(CGImageGetBitsPerComponent(decoded));
  input.bitsPerPixel = static_cast<uint32_t>(bits);
  input.colorSpace = CGImageGetColorSpace(decoded);
  input.bitmapInfo = CGImageGetBitmapInfo(decoded);
  input.decode = CGImageGetDecode(decoded);
  input.renderingIntent = CGImageGetRenderingIntent(decoded);
  // Read and convert the native raster. CGContextDrawImage and the drawing
  // fallback in vImageBuffer_InitWithCGImage leave RIP/ColorSync allocations
  // reported by LSan even for valid PNGs.
  vImage_Error error = kvImageNoError;
  vImageConverterRef converter = vImageConverter_CreateWithCGImageFormat(
      &input, &format, nullptr, kvImageNoFlags, &error);
  bool converted = false;
  if (converter) {
    vImage_Buffer source_buffer = {const_cast<UInt8*>(CFDataGetBytePtr(pixels)),
                                   static_cast<vImagePixelCount>(height),
                                   static_cast<vImagePixelCount>(width),
                                   stride};
    error = vImageConvert_AnyToAny(converter, &source_buffer, &buffer, nullptr,
                                   kvImageNoFlags);
    converted = error == kvImageNoError;
    vImageConverter_Release(converter);
  }
  CGColorSpaceRelease(color);
  CFRelease(pixels);
  CGImageRelease(decoded);
  if (!converted) {
    return false;
  }
  *image = std::move(bitmap);
  return true;
}
}  // namespace lychee
