//
// Created by boyan on 2021/2/16.
//

#include "render_video_sdl.h"
#include "sdl_utils.h"

SdlVideoRender::SdlVideoRender(std::shared_ptr<SDL_Renderer> renderer)
    : VideoRenderBase(), renderer_(std::move(renderer)) {

}

void SdlVideoRender::RenderPicture(Frame &frame) {
  SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255);
  SDL_RenderClear(renderer_.get());
  SDL_Rect rect{};
  media::sdl::calculate_display_rect(&rect, 0, 0, screen_width,
                                     screen_height, frame.width, frame.height, frame.sar);
  if (!frame.uploaded) {
    if (UploadTexture(frame.frame) < 0) {
      return;
    }
    frame.uploaded = 1;
    frame.flip_v = frame.frame->linesize[0] < 0;
  }

  SetSdlYuvConversionMode(frame.frame);
  SDL_RenderCopyEx(renderer_.get(), texture_, nullptr, &rect, 0, nullptr,
                   frame.flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
  SetSdlYuvConversionMode(nullptr);
  SDL_RenderPresent(renderer_.get());
}

SdlVideoRender::~SdlVideoRender() {
  if (texture_) {
    SDL_DestroyTexture(texture_);
  }
  if (sub_texture_) {
    SDL_DestroyTexture(sub_texture_);
  }
  sws_freeContext(img_convert_ctx);
}

int SdlVideoRender::UploadTexture(AVFrame *frame) {
  int ret = 0;
  Uint32 sdl_pix_fmt;
  SDL_BlendMode sdl_blendmode;
  GetSdlPixFmtAndBlendMode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
  if (ReAllocTexture(sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt,
                     frame->width, frame->height, sdl_blendmode, 0) < 0)
    return -1;
  switch (sdl_pix_fmt) {
    case SDL_PIXELFORMAT_UNKNOWN: {
      static int sws_flags = SWS_BICUBIC;
      /* This should only happen if we are not using avfilter... */
      img_convert_ctx = sws_getCachedContext(img_convert_ctx,
                                             frame->width, frame->height, AVPixelFormat(frame->format),
                                             frame->width, frame->height, AVPixelFormat::AV_PIX_FMT_BGRA,
                                             sws_flags, nullptr, nullptr, nullptr);

      if (img_convert_ctx != nullptr) {
        uint8_t *pixels[4];
        int pitch[4];
        if (!SDL_LockTexture(texture_, nullptr, (void **) pixels, pitch)) {
          sws_scale(img_convert_ctx, (const uint8_t *const *) frame->data, frame->linesize,
                    0, frame->height, pixels, pitch);
          SDL_UnlockTexture(texture_);
        }
      } else {
        av_log(nullptr, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
        ret = -1;
      }
    }
      break;
    case SDL_PIXELFORMAT_IYUV:
      if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
        ret = SDL_UpdateYUVTexture(texture_, nullptr, frame->data[0], frame->linesize[0],
                                   frame->data[1], frame->linesize[1],
                                   frame->data[2], frame->linesize[2]);
      } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
        ret = SDL_UpdateYUVTexture(texture_, nullptr, frame->data[0] + frame->linesize[0] * (frame->height - 1),
                                   -frame->linesize[0],
                                   frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                                   -frame->linesize[1],
                                   frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                                   -frame->linesize[2]);
      } else {
        av_log(nullptr, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
        return -1;
      }
      break;
    default:
      if (frame->linesize[0] < 0) {
        ret = SDL_UpdateTexture(texture_, nullptr, frame->data[0] + frame->linesize[0] * (frame->height - 1),
                                -frame->linesize[0]);
      } else {
        ret = SDL_UpdateTexture(texture_, nullptr, frame->data[0], frame->linesize[0]);
      }
      break;
  }
  return ret;
}

void SdlVideoRender::GetSdlPixFmtAndBlendMode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode) {
  static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
  } sdl_texture_format_map[] = {
      {AV_PIX_FMT_RGB8, SDL_PIXELFORMAT_RGB332},
      {AV_PIX_FMT_RGB444, SDL_PIXELFORMAT_RGB444},
      {AV_PIX_FMT_RGB555, SDL_PIXELFORMAT_RGB555},
      {AV_PIX_FMT_BGR555, SDL_PIXELFORMAT_BGR555},
      {AV_PIX_FMT_RGB565, SDL_PIXELFORMAT_RGB565},
      {AV_PIX_FMT_BGR565, SDL_PIXELFORMAT_BGR565},
      {AV_PIX_FMT_RGB24, SDL_PIXELFORMAT_RGB24},
      {AV_PIX_FMT_BGR24, SDL_PIXELFORMAT_BGR24},
      {AV_PIX_FMT_0RGB32, SDL_PIXELFORMAT_RGB888},
      {AV_PIX_FMT_0BGR32, SDL_PIXELFORMAT_BGR888},
      {AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888},
      {AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888},
      {AV_PIX_FMT_RGB32, SDL_PIXELFORMAT_ARGB8888},
      {AV_PIX_FMT_RGB32_1, SDL_PIXELFORMAT_RGBA8888},
      {AV_PIX_FMT_BGR32, SDL_PIXELFORMAT_ABGR8888},
      {AV_PIX_FMT_BGR32_1, SDL_PIXELFORMAT_BGRA8888},
      {AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_IYUV},
      {AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2},
      {AV_PIX_FMT_UYVY422, SDL_PIXELFORMAT_UYVY},
      {AV_PIX_FMT_NONE, SDL_PIXELFORMAT_UNKNOWN},
  };

  int i;
  *sdl_blendmode = SDL_BLENDMODE_NONE;
  *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
  if (format == AV_PIX_FMT_RGB32 ||
      format == AV_PIX_FMT_RGB32_1 ||
      format == AV_PIX_FMT_BGR32 ||
      format == AV_PIX_FMT_BGR32_1)
    *sdl_blendmode = SDL_BLENDMODE_BLEND;
  for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
    if (format == sdl_texture_format_map[i].format) {
      *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
      return;
    }
  }
}

void SdlVideoRender::SetSdlYuvConversionMode(AVFrame *frame) {
#if SDL_VERSION_ATLEAST(2, 0, 8)
  SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
  if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 ||
      frame->format == AV_PIX_FMT_UYVY422)) {
    if (frame->color_range == AVCOL_RANGE_JPEG)
      mode = SDL_YUV_CONVERSION_JPEG;
    else if (frame->colorspace == AVCOL_SPC_BT709)
      mode = SDL_YUV_CONVERSION_BT709;
    else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M ||
        frame->colorspace == AVCOL_SPC_SMPTE240M)
      mode = SDL_YUV_CONVERSION_BT601;
  }
  SDL_SetYUVConversionMode(mode);
#endif
}

int SdlVideoRender::ReAllocTexture(
    Uint32 new_format,
    int new_width,
    int new_height,
    SDL_BlendMode blendmode,
    int init_texture
) {

  Uint32 format;
  int access, w, h;
  if (!texture_ || SDL_QueryTexture(texture_, &format, &access, &w, &h) < 0 || new_width != w || new_height != h ||
      new_format != format) {
    void *pixels;
    int pitch;
    if (texture_)
      SDL_DestroyTexture(texture_);
    if (!(texture_ = SDL_CreateTexture(renderer_.get(), new_format, SDL_TEXTUREACCESS_STREAMING, new_width,
                                       new_height)))
      return -1;
    if (SDL_SetTextureBlendMode(texture_, blendmode) < 0)
      return -1;
    if (init_texture) {
      if (SDL_LockTexture(texture_, nullptr, &pixels, &pitch) < 0)
        return -1;
      memset(pixels, 0, pitch * new_height);
      SDL_UnlockTexture(texture_);
    }
    av_log(nullptr, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height,
           SDL_GetPixelFormatName(new_format));
  }
  return 0;
}

void SdlVideoRender::DestroyTexture() {
  SDL_DestroyTexture(texture_);
  texture_ = nullptr;
}

