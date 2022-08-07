#ifndef FLUTTER_PLUGIN_AUDIO_PLAYER_PLUGIN_H_
#define FLUTTER_PLUGIN_AUDIO_PLAYER_PLUGIN_H_

#include <flutter_plugin_registrar.h>
#include <flutter/plugin_registrar_windows.h>

#ifdef FLUTTER_PLUGIN_IMPL
#define FLUTTER_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FLUTTER_PLUGIN_EXPORT __declspec(dllimport)
#endif

#if defined(__cplusplus)
extern "C" {
#endif

FLUTTER_PLUGIN_EXPORT void MediaPlayerPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar);

//extern "C" void register_flutter_plugin(flutter::PluginRegistrarWindows *registrar);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // FLUTTER_PLUGIN_AUDIO_PLAYER_PLUGIN_H_
