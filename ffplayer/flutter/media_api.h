//
// Created by yangbin on 2021/5/16.
//

#ifndef MEDIA_FLUTTER_MEDIA_API_H_
#define MEDIA_FLUTTER_MEDIA_API_H_

#include "cinttypes"
#include "memory"

#ifdef _WIN32
#define API_EXPORT extern "C"  __declspec(dllexport)
#else
#define API_EXPORT extern "C" __attribute__((visibility("default"))) __attribute__((used))
#endif

class FlutterMediaTexture {

 public:

  enum PixelFormat {
    /**
     * for macos.
     */
    kFormat_32_BGRA,
  };

  virtual int64_t GetTextureId() = 0;

  virtual void Release() = 0;

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

  virtual void LockBuffer() = 0;

  virtual void UnlockBuffer() = 0;

  virtual void NotifyBufferUpdate() = 0;

  /**
   * @return The pixel buffer which init by [MaybeInitPixelBuffer], could be null.
   */
  virtual uint8_t *GetBuffer() = 0;

  virtual ~FlutterMediaTexture() = default;

};

typedef std::unique_ptr<FlutterMediaTexture>(*FlutterTextureAdapterFactory)();

API_EXPORT void register_flutter_texture_factory(FlutterTextureAdapterFactory factory);

#endif //MEDIA_FLUTTER_MEDIA_API_H_
