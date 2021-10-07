//
// Created by yangbin on 2021/5/16.
//

#ifndef MEDIA_FLUTTER_EXTERNAL_MEDIA_TEXTURE_H_
#define MEDIA_FLUTTER_EXTERNAL_MEDIA_TEXTURE_H_

#include "cinttypes"
#include "memory"
#include "functional"

#ifdef _WIN32
#define API_EXPORT extern "C"  __declspec(dllexport)
#else
#define API_EXPORT extern "C" __attribute__((visibility("default"))) __attribute__((used))
#endif

class ExternalMediaTexture {

 public:

  enum PixelFormat {
    kFormat_32_BGRA,
    kFormat_32_ARGB,
    kFormat_32_RGBA,
  };

  virtual int64_t GetTextureId() = 0;

  /**
   * @param width the video frame width
   * @param height the video frame height
   */
  virtual void MaybeInitPixelBuffer(int width, int height) = 0;

  virtual int GetWidth() = 0;

  virtual int GetHeight() = 0;

  /**
   * @return the format current platform supported.
   */
  virtual PixelFormat GetSupportFormat() = 0;

  virtual bool TryLockBuffer() = 0;

  virtual void UnlockBuffer() = 0;

  virtual void NotifyBufferUpdate() = 0;

  /**
   * @return The pixel buffer which init by [MaybeInitPixelBuffer], could be null.
   */
  virtual uint8_t *GetBuffer() = 0;

  virtual ~ExternalMediaTexture() = default;

  virtual void RenderWithHWAccel(void *pixel_buffer) {}

};

typedef void(*FlutterTextureAdapterFactory)(std::function<void(std::unique_ptr<ExternalMediaTexture>)> callback);

API_EXPORT void register_external_texture_factory(FlutterTextureAdapterFactory factory);

#endif //MEDIA_FLUTTER_EXTERNAL_MEDIA_TEXTURE_H_
