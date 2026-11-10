# Android TV SurfaceView composition

Investigated on 2026-11-10, on a TCL MT9655 TV, Android 14, 60 Hz,
with Flutter 3.47.8 profile builds and MediaCodec hardware decoding.

## Result

The native video underlay now reaches the TV's DEVICE(MM) video plane
and delivers about 60 frames per playback second for hevc10_4k60.
The decisive change is configuring a fresh decoder with the native
SurfaceView attached. Switching an already configured decoder from a
Flutter SurfaceTexture with setOutputSurface alone left video in CLIENT
composition on this TV.

The plugin therefore requests one decoder rebuild after the new hardware
video Surface is created and attached. It uses the existing C ABI recovery
operation, through a small Android SDK bridge for C-ABI-created players.
The core preserves the playback state and resynchronizes at the current
position. No new Dart setting or C ABI is needed. Changing the rectangle
does not rebuild the decoder; software output does not request a rebuild.

This identifies a working intervention on this device. It does not reveal
the vendor driver's internal allocation or overlay-selection algorithm.
Android requires compatible usage types when changing output surfaces;
a successful switch is not evidence that all decoder setup decisions
have been repeated for a different consumer.
[MediaCodec.setOutputSurface](https://developer.android.com/reference/android/media/MediaCodec#setOutputSurface(android.view.Surface))

## Why flutter-airplay mattered

flutter-airplay already creates a native video SurfaceView below a
TextureView-backed FlutterView. Its Android host prepares that Surface
before reception starts. Video goes directly to it; RenderMode.texture
names the UI's rendering mode, not a Flutter texture containing video.

The earlier TV capture in flutter-airplay's ignored
`artifacts/tv-debug-20261023/composer-4k-playing.txt` shows a 3840x2160
video source in DEVICE(MM), with a separate 1920x1080 UI layer in DEVICE.
Its H.264 arrival/decoding/release samples were roughly 230 frames per
five seconds, so that capture establishes the 4K hardware video path,
not sustained 4K60 input or presentation.

The lychee Flutter example instead creates a SurfaceTexture immediately.
It opens media before LycheeVideo knows the video size. Only after frames
arrive can the widget size its native view and switch the existing
MediaCodec to its Surface. The installed native SDK example normally had
its native Surface attached by codec configuration, without this detour.

```text
Before:
MediaCodec configured for SurfaceTexture
    -> setOutputSurface(SurfaceView) -> CLIENT video composition

After:
SurfaceView created and attached
    -> fresh MediaCodec configuration -> DEVICE(MM) video composition

Layers, front to back:
Flutter UI in TextureView, with a transparent video rectangle
Native video SurfaceView, with MediaCodec output
```

## Controlled experiment

The same probe APK used launch extras to change only whether the native
Surface was attached before opening the media. The early-attachment
probe waited for the native view before opening; its temporary 500 ms
wait is not part of the fix. The normal plugin waits for the actual
SurfaceHolder surfaceCreated callback instead.

The media, decoder, Flutter background, video rectangle and frame driver
were unchanged. The video sampled the full 3840x2160 source into display
coordinates `[455,232,1465,800]`.

| Configuration | Rendered | Dropped | Position (s) | Rendered / position | Video composition |
| --- | ---: | ---: | ---: | ---: | --- |
| Probe: open first, then attach native Surface | 481 | 72 | 10.539 | 45.6 | CLIENT |
| Same probe: native Surface before open | 637 | 3 | 10.497 | 60.7 | DEVICE(MM) |
| Fixed normal Flutter example, ordinary open order | 589 | 6 | 9.720 | 60.6 | DEVICE(MM) |

Each run sampled SurfaceFlinger at 3, 6 and 9 seconds. The native video
layer was present in every sample, including its 4K source crop. The
successful probe kept Flutter's existing clear-rectangle painting, so a
new background strategy was unnecessary. The fixed normal example had
its ordinary controls and ongoing UI updates.

Additional checks with the fixed normal example:

| Media | Rendered | Dropped | Position (s) | Video composition |
| --- | ---: | ---: | ---: | --- |
| H.264 3840x2160 at 30 fps | 299 | 0 | 9.781 | DEVICE(MM) |
| HDR PQ 1280x720 at 30 fps, three-second clip | 89 | 1 | 3.000 | DEVICE(MM) |

The HDR layer retained BT2020_ITU_PQ and static HDR metadata. This checks
HDR handoff to the compositor, not optical brightness, gamut accuracy or
one-pixel preservation on the physical panel. The 4K60 timing clip has no
color-primary, matrix or transfer tags, so its timing results alone do
not establish HDR behavior.

The new `tool/android_surface_check.py` also passed on the normal example:
572 rendered, 7 dropped, position 9.457 s, and DEVICE at all three samples.
A separate steady-playback SurfaceFlinger latency capture had 126 distinct
actual-present timestamps, about 58.6/s, with a median interval of 16.67 ms
and maximum interval of 33.35 ms. This is the compositor's reported
presentation cadence, not an optical panel measurement. AOSP's second
latency-dump column is actualPresentTime.
[FrameTracker::dumpStats](https://android.googlesource.com/platform/frameworks/native/+/ee4bf4c9660809e7fb967cccfd6bdcfc67b3b113/services/surfaceflinger/FrameTracker.cpp)

A temporary profile harness switched SurfaceView -> texture -> SurfaceView
in one player. The native plane returned after reattachment, frame counts
resumed increasing, and disposal left zero lychee threads. Switching back
caused about 1-2 seconds of rebuffering on this TV while the decoder was
recreated. This transition cost is separate from steady playback cadence.
The normal example's video placement and controls were also checked in a
separate screenshot run.

## Earlier negative experiments

These experiments established the symptom but were insufficient fixes:

- An explicitly transparent FlutterTextureView still produced CLIENT
  video, about 45 acquired frames per playback second.
- A transparent FlutterSurfaceView above the video still produced CLIENT
  for video, Activity window and Flutter surface, about 42 acquired
  frames per playback second.
- Valid static Flutter pages drew only five startup frames and then
  stopped repainting. Video still stayed CLIENT at about 43-45 acquired
  frames per playback second. Continuous UI repainting was not required
  for the failure.

A foreground graphics trace of the failing static surface-mode page had
267 complete Choreographer callbacks over 4.984 seconds, about 53.4/s,
with 33 gaps over 25 ms. Callback duration was 1.29 ms median, 13.23 ms
maximum. It contained 215 HEVC codec work-done events. A matching native
SDK trace had about 59.8 callbacks/s and 300 work-done events. These
symptoms did not by themselves distinguish buffer delivery from composer
policy; the initialization experiment above provided that distinction.

A blank page, missing video layer or screen-saver layer invalidates a
trial. Earlier screen-saver traces had smooth background statistics and
were rejected. They were not evidence of smooth visible video.

## Reading the measurements

`rendered` counts frames handed out by VideoRenderer::Acquire(), not
verified panel presentations. `dropped` counts frames skipped during
selection; frames expired while hidden are not included. A smaller drop
count alone is insufficient. The acquisition ratio is approximate because
position sampling and startup differ, and can slightly exceed the source
frame rate. These are individual diagnostic runs, not a benchmark
confidence interval.

DEVICE for the final Flutter window does not mean decoded video used a
native plane: Flutter may already have sampled a video texture into that
window. Check the separate video SurfaceView and its full source crop.
The DEVICE(MM) label here comes from the TV vendor's composer dump.
[AOSP composition contract](https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/master/graphics/composer/aidl/android/hardware/graphics/composer3/Composition.aidl)

Likewise, `present fence has not signaled` identifies delayed presentation
synchronization, not its cause. AOSP prints the message after a 100 ms
wait. It is not sufficient proof of GPU saturation.
[TransactionCompletedThread.cpp](https://android.googlesource.com/platform/frameworks/native/+/dd81befb570dfda0e20b5c9a6c233ab27f1c3d93/services/surfaceflinger/TransactionCompletedThread.cpp)

## Repeating the regression check

Install a profile Flutter example built with
`--dart-define=LYCHEE_SURFACE_VIEW=true`, using the checkout's Android SDK
(`sdk/android/gradlew -p sdk/android :lychee:publish`). Supply media the
example can open under its network security configuration. For a local
HTTP server, establish adb reverse and explicitly allow that HTTP traffic
in the test app; the temporary test manifest is not shipped by this fix.

```sh
adb -s DEVICE reverse tcp:8765 tcp:8765
python3 -I tool/android_surface_check.py --device DEVICE \
  --url http://127.0.0.1:8765/hevc10_4k60.mp4 \
  --out /tmp/lychee-surface-check
```

The command fails if it cannot identify the visible native 4K video
layer, any sample is CLIENT, the acquisition ratio is below 54/s, or
selection drops exceed 5%. The thresholds require a 60 fps clip and a
capable real device; lower-rate inputs need the matching --size and
--min-fps. They do not make an emulator a TV hardware-composer test.

Original evidence is local: `/tmp/lychee-tv-research/` holds the initial
experiments, while `/tmp/lychee-tv-compare/` holds the controlled probe,
fixed-example runs, SurfaceFlinger dumps, process logs and temporary probe
sources. Logs may contain VM-service addresses; redact before publishing.
