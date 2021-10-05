//
// Created by boyan on 10/5/21.
//

#ifndef LYCHEE_PLAYER_FL_MEDIA_TEXTURE_H_
#define LYCHEE_PLAYER_FL_MEDIA_TEXTURE_H_

#include <flutter_linux/flutter_linux.h>

/// A simple texture.
struct _FlMediaTexture {
  FlTextureGL parent_instance;
  int64_t texture_id = 0;
  uint8_t *buffer = nullptr;
  int32_t height = 0;
  int32_t width = 0;
};

G_DECLARE_FINAL_TYPE(FlMediaTexture, fl_media_texture, FL, MEDIA_TEXTURE, FlPixelBufferTexture)

G_DEFINE_TYPE(FlMediaTexture, fl_media_texture, fl_pixel_buffer_texture_get_type())

static gboolean fl_test_pixel_buffer_texture_copy_pixels(
    FlPixelBufferTexture *texture,
    const uint8_t **out_buffer,
    uint32_t *width,
    uint32_t *height,
    GError **error) {

  *out_buffer = FL_MEDIA_TEXTURE(texture)->buffer;
  *width = FL_MEDIA_TEXTURE(texture)->height;
  *height = FL_MEDIA_TEXTURE(texture)->width;

  return true;
}


void fl_media_texture_init(FlMediaTexture *self) {}

void fl_media_texture_class_init(FlMediaTextureClass *klass) {
  FL_PIXEL_BUFFER_TEXTURE_CLASS(klass)->copy_pixels =
      fl_test_pixel_buffer_texture_copy_pixels;
}

FlMediaTexture *fl_media_texture_new() {
  return FL_MEDIA_TEXTURE(g_object_new(fl_media_texture_get_type(), nullptr));
}

#endif //LYCHEE_PLAYER_FL_MEDIA_TEXTURE_H_
