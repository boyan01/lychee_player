//
// Created by Bin Yang on 2021/9/19.
//

#include "media_player_controller.h"
#include "macos_audio_renderer_sink.h"
#include "external_video_renderer_sink.h"

#include "skia_media_texture.h"

namespace {
auto playerLooper = media::base::MessageLooper::PrepareLooper("media_player");
}

MediaPlayerController::MediaPlayerController()
    : player_() {

  player_ = std::make_shared<media::MediaPlayer>(
      std::make_unique<media::ExternalVideoRendererSink>(),
      std::make_shared<media::MacosAudioRendererSink>(),
      media::TaskRunner(playerLooper));

//  player_->OpenDataSource("/Users/yangbin/Movies/badapple_full.mp4");
//  player_->OpenDataSource("/Users/yangbin/Movies/LG-Rays-of-Light-4K.ts");
  player_->OpenDataSource("/Users/yangbin/Movies/good_words.mp4");
  player_->SetPlayWhenReadyTask(true);

//  const gfx::Rect rc = surface_->bounds();
//
//  os::Paint p;
//  p.color(gfx::rgba(0xff, 0xff, 0xff));
//  p.style(os::Paint::Fill);
//  p.antialias(true);
//
//  os::draw_text(surface_.get(), nullptr, "Hello World1", rc.center(),
//                &p, os::TextAlign::Center);

}

void MediaPlayerController::Render(os::Surface *surface) {
  auto texture_id = dynamic_cast<media::ExternalVideoRendererSink *>(player_->GetVideoRenderSink())->texture_id();
  auto external_surface = SkiaMediaTexture::GetExternalSurface(texture_id);

  if (!external_surface || !external_surface->isValid()) {
    return;
  }

  surface->lock();
  surface->drawSurface(external_surface.get(),
                       external_surface->bounds(),
                       external_surface->bounds().fitIn(surface->bounds()));
  surface->unlock();
}
