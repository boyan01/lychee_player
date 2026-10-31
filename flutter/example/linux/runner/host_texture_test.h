#ifndef RUNNER_HOST_TEXTURE_TEST_H_
#define RUNNER_HOST_TEXTURE_TEST_H_

#include <flutter_linux/flutter_linux.h>

// With LYCHEE_HOST_TEXTURE_TEST=FILE, exercises lychee_host_texture.h as an
// app with its own players would. Once Dart shows the texture (it calls
// "start" on the lychee_example/host_texture channel, which returns the
// texture id), a new player playing FILE replaces the shown one every second,
// a few times; then the texture is unset and freed. Each player is destroyed
// as soon as set_player() lets go of it. Prints "host_texture_test: ok" with
// the frames the players rendered.
void host_texture_test_start(FlPluginRegistry* registry);

#endif  // RUNNER_HOST_TEXTURE_TEST_H_
