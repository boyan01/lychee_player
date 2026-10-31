#ifndef RUNNER_HOST_TEXTURE_TEST_H_
#define RUNNER_HOST_TEXTURE_TEST_H_

#include <flutter/flutter_engine.h>

// With LYCHEE_HOST_TEXTURE_TEST=FILE, exercises lychee_host_texture.h as an
// app with its own players would. Once Dart shows the texture (it calls
// "start" on the lychee_example/host_texture channel, which returns the
// texture id), a new player playing FILE replaces the shown one every second,
// a few times; then the texture is unset and destroyed. Each player is
// destroyed as soon as SetPlayer() lets go of it. Prints
// "host_texture_test: ok" with the frames the players rendered.
void StartHostTextureTest(flutter::FlutterEngine* engine);

#endif  // RUNNER_HOST_TEXTURE_TEST_H_
