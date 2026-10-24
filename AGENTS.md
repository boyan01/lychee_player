# AGENTS.md

lychee_player is a media player built as a reusable C++ library with a C ABI
(`core/`), plus a Flutter plugin that is one thin consumer of it. README.md
has the layout and build commands.

Before starting a feature or a new platform, read `docs/roadmap.md`: it lists
what is unfinished, known defects, and the done criteria for each item. Update
it in the same change when you finish, start, or discover an item.

## Design rules

These are decisions, not accidents. Keep them unless the user changes them.

- **The C ABI is the product.** `core/include/lychee/lychee.h` is the only
  public header. Everything crossing it is a plain C type: opaque handles,
  structs that start with `struct_size` so they can grow, callbacks with
  plain integer arguments (so Dart `NativeCallable.listener` can bind them).
  The core never depends on Flutter or any UI toolkit.
- **FFmpeg is an optional backend.** FFmpeg types and headers appear only
  under `core/src/backends/ffmpeg/`. `media/` and `pipeline/` use the
  library's own types (`EncodedPacket`, `VideoConfig`, `VideoFrame`, ...).
  Backends are chosen at build time in `media/factory.cc`.
- **Frames leave as native handles** (`CVPixelBufferRef` today; D3D11
  textures, dmabuf, AHardwareBuffer later). The library never renders. The
  caller pulls frames per vsync with `has_new_frame` / `acquire_frame`, in
  the `lychee_now_us()` timebase; the library picks the frame for that time.
- **License hygiene.** The project is MIT. FFmpeg must stay an LGPL-only
  build (no `--enable-gpl`, no `--enable-nonfree`), linked dynamically.
  Write code from your own understanding of ffplay or mpv rather than copying
  it: both are (L)GPL. Code ported from Chromium is BSD-3 and keeps its
  copyright header.
- **Name the borrowing.** When porting a Chromium algorithm, say which file it
  comes from in a comment, so later readers can compare.

## Concurrency

The pipeline is multithreaded; most bugs here are lifetime and ordering bugs.

- Threads: the control thread (`Player::Run`: open, seeks, state machine,
  events), the read thread (`Player::ReadLoop`: only `Demuxer::Read`, so a
  stalled network read blocks nothing else), one decode thread per renderer, the CoreAudio render thread, and whatever
  thread the caller pulls frames on.
- Lock order is player → renderer → clock, and audio renderer → clock.
  Renderers never call back into the player.
- A seek interrupts the read in flight (`Demuxer::Interrupt`) and parks the
  read thread while the control thread seeks the demuxer.
- A seek bumps a **generation**. Packets and frames carry the generation they
  belong to; anything from an older generation is dropped, and a decoder is
  flushed when it first sees a newer one.
- Stopping a renderer requires aborting its `PacketQueue` first, or the
  decode thread stays blocked in `Pop`.
- Flutter teardown order matters: `disposeTexture` (stops the display link,
  nulls the player under the texture's lock) → `lychee_player_destroy`
  (joins all threads, so no more callbacks) → `NativeCallable.close()`.

## Verifying a change

`tool/regression.sh` builds the core and plays a fixed media matrix through
`examples/cli` (headless, simulated 60 Hz vsync, muted). It prints
PASS/FAIL per case. Done means:

- `tool/regression.sh` passes with zero build warnings.
- For changes to `pipeline/`, threading, or teardown: `TSAN=1
  tool/regression.sh` also passes (it fails on any ThreadSanitizer report).
- For plugin or Dart changes: `flutter analyze` in `flutter/` is clean, and
  you have run the example (`cd flutter/example && flutter build macos --debug
  --dart-define=LYCHEE_URL=/tmp/lychee-media/h264_aac.mp4`, launch the app
  binary) and looked at a window screenshot. Say in your report what you
  did not see, e.g. audio you could not hear.
- For changes to player lifetime or teardown (Dart, plugin, or core):
  `cd flutter/example && flutter test integration_test -d macos` also
  passes. It switches files repeatedly and checks that exactly one player's
  threads remain.
- For changes to what ends up on screen (frame selection, conversion,
  rotation): run `build/examples/glfw/lychee_glfw` on a test file and look at
  a window screenshot. It has no Flutter in between, so it also tells core
  bugs from texture bridge bugs.
- A new behaviour gets a case in `tool/regression.sh` when the CLI can
  observe it.

## Gotchas

- After adding or removing a `.cc` under `core/src`, run
  `python3 tool/generate_forwarders.py`. The macOS pod compiles the core only
  through those forwarders and silently misses new files otherwise.
- Run CocoaPods with `LANG=en_US.UTF-8`, or `pod install` aborts.
- `flutter test integration_test -d macos` builds into the same
  `build/macos/.../lychee_player_example.app` as `flutter build macos`, with
  the test as its entry point; launched by hand it shows a blank window.
  Rebuild the example before handing the binary to anyone.
- Asking VideoToolbox (through FFmpeg) for BGRA output fails at hwaccel init
  and FFmpeg quietly falls back to software decoding. Keep its default output
  and convert with `AppleFrameConverter`. Check `hw=1` in the CLI summary
  after touching decoder setup.
- Homebrew's FFmpeg is a GPL build and is linked only for development; the
  `ld: building for macOS-12.0` warnings in the example build come from it.
- The default output device on the development Mac is a virtual device that
  reports zero latency, so `latency 0 us` in the CoreAudio log is expected
  there.
