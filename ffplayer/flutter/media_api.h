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

  virtual int64_t GetTextureId() = 0;

  virtual void Release() = 0;

  virtual void Render(uint8_t *data[8], int line_size[8], int width, int height) = 0;

  virtual int GetWidth() = 0;

  virtual int GetHeight() = 0;

  virtual ~FlutterMediaTexture() = default;

};

typedef std::unique_ptr<FlutterMediaTexture>(*FlutterTextureAdapterFactory)();

API_EXPORT void register_flutter_texture_factory(FlutterTextureAdapterFactory factory);

#endif //MEDIA_FLUTTER_MEDIA_API_H_
