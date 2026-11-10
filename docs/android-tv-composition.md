# Android TV video composition investigation

Research date: 2026-11-10. Scope: the TCL/MediaTek TV's poor 4K60
playback with a native video SurfaceView below Flutter. This note separates
the supplied measurements, new TV experiments, source-confirmed behavior,
and hypotheses. It does not establish the TV vendor's overlay-selection policy.

## Conclusion

The proposed transparent FlutterSurfaceView was tested on the TV. It did
not restore hardware video composition. Its video, Activity window and
Flutter surface all remained CLIENT. Explicitly transparent
FlutterTextureView also failed. Both configurations still failed with a
valid static Flutter page that stopped drawing after five startup frames.
These changes are therefore insufficient fixes on this TV.

The failure is narrowed to the native video SurfaceView's interaction
with this Flutter scene: the same probe APK's Flutter video texture path
remained smooth, and the installed native SDK example used DEVICE(MM).
The precise vendor-composer rejection or display-scheduling cause remains
unresolved. Neither CLIENT nor an unsignaled present fence alone proves
that GPU throughput is the root cause.

## Evidence and its limits

The user supplied these approximately 9.5-second `hevc10_4k60` runs:

| Path | Rendered | Dropped | Reported composition |
| --- | --- | --- | --- |
| Flutter texture, same APK | 543 / 589 / 594 / 588 | 19 / 0 / 1 / 3 | DEVICE |
| Video SurfaceView below Flutter TextureView | 414 / 438 / 442 / 427 | 85 / 101 / 105 / 89 | CLIENT |
| Earlier Hybrid Composition SurfaceView | about 400 | 25–42 | Not established |
| Native SDK SurfaceView | 605 | 15 | DEVICE(MM) |

These are reported observations, not new measurements made for this note.
The native SDK result shows that this hardware can play this clip well
through a SurfaceView. It does not isolate the reason for the Flutter
configuration's failure. The earlier failed static-page experiment supplies no
evidence about whether Flutter UI updates cause the fallback.

## New TV experiments

Device: TCL MT9655, Android API 34, 60 Hz. Flutter 3.47.8, profile build,
arm64, MediaCodec hardware decoding. The same local `hevc10_4k60.mp4`
was served through adb reverse. It is 3840x2160 at 60 fps; ffprobe reports
no color-primary, transfer or matrix tags. This test does not establish
HDR correctness or HDR-mode behavior.

| Probe | Rendered | Dropped | Position (s) | Rendered / position | Composition |
| --- | ---: | ---: | ---: | ---: | --- |
| Existing underlay APK, reproduction | 419 | 72 | 9.377 | 44.7 | Video + window CLIENT |
| Same probe APK, opaque TextureView host | 424 | 73 | 9.260 | 45.8 | Video + window CLIENT |
| Same probe APK, transparent TextureView host | 432 | 104 | 9.533 | 45.3 | Video + window CLIENT |
| Same probe APK, transparent SurfaceView host | 408 | 21 | 9.639 | 42.3 | Video + window + Flutter CLIENT |
| Same probe APK, video routed through Flutter texture | 584 | 4 | 9.613 | 60.8 | Final window DEVICE; no native video layer |
| Static-page APK, transparent SurfaceView host | 431 | 15 | 10.098 | 42.7 | Video + window + Flutter CLIENT |
| Static-page APK, transparent TextureView host | 458 | 103 | 10.117 | 45.3 | Video + window CLIENT |
| Installed native SDK example, reference | 599 | 1 | 10.002 | 59.9 | Video DEVICE(MM) |

These are individual diagnostic runs, not a benchmark distribution.
The dynamic configurations used one APK and changed only launch extras.
Native video bounds remained `[455,232,1465,800]`, sampling the full
3840x2160 source. SurfaceFlinger was sampled three times per Flutter run,
at 3, 6 and 9 seconds after launch completion. Its native video layer was
present in every underlay trial; the surface-mode trial had two distinct
SurfaceViews and the intervening Activity window. The native SDK example
used a larger video rectangle, so it is a capability reference rather
than a geometrically matched control.

The static page had Directionality, an opaque background, and a bounded
LycheeVideo in the same rectangle. FrameTiming callbacks counted five
Flutter frames in total; once-per-second logs remained at five from the
two-second sample through the end in both host modes. The static-surface
page was also captured in a separate visual-only run to confirm that the
video was visible and correctly placed. That capture was excluded from
the performance table.

`rendered` counts frames handed out by VideoRenderer::Acquire(), not
verified panel presentations. `dropped` counts frames skipped during
selection; frames expired while hidden are not counted. A smaller drop
count alone is therefore insufficient: the transparent SurfaceView
reported only 15 drops on the static page but handed out about 43 frames
per playback second. The ratio is approximate because position sampling
and startup differ; it is not a panel frame-rate measurement. See
[VideoRenderer counts](../core/src/pipeline/video_renderer.h) and
[selection and acquisition](../core/src/pipeline/video_renderer.cc).

The native example also updates its status TextView every 500 ms; it is
not a completely static UI. See its
[tick and update functions](../sdk/android/example/src/main/kotlin/tech/soit/lychee/example/MainActivity.kt).
Thus any claim that merely updating UI above video necessarily prevents
hardware composition is too broad. The static Flutter results further
show that continuous Flutter repainting is not required for this failure.

Temporary probes explicitly overrode both getRenderMode() and
getTransparencyMode(), allowed FlutterSurfaceView in the plugin gate,
and logged the selected modes. The probe manifest allowed HTTP for the
local media server. The first HTTP-refused run was rejected as invalid.
The production working-tree files were restored byte-for-byte afterward;
no runtime fix was retained. ADB package installation was refused by the
TV, so the user confirmed installation through its system installer.

Local evidence is in `/tmp/lychee-tv-research/`: per-run SurfaceFlinger
dumps and process logs, a `run.py` driver, both probe APKs, probe source
copies and `static.dart`. The driver fails when the acquisition ratio is
below 54 or selection drops exceed 5%; it was observed failing on the
underlay and passing on the texture control. These are investigation
thresholds, not product acceptance criteria. Logs can contain VM-service
addresses; do not publish them unredacted.

## Interpreting compositor evidence

`CLIENT` means the layer is composed into the client target. `DEVICE`
means the composer handles that layer using an overlay or similar means.
This is a per-layer composition decision, not a video decoder mode.
Consequently, DEVICE for the Flutter texture path's final window does not
mean that the original decoded video reached a video plane directly:
Flutter may already have sampled the video into its output. See the
[composer composition contract](https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/master/graphics/composer/aidl/android/hardware/graphics/composer3/Composition.aidl).

The exact AOSP log `present fence has not signaled, err %d` occurs after
a 100 ms wait on a present fence. That fence describes the composed
display frame reaching presentation, rather than an individual GPU task.
The log identifies delayed or failed presentation synchronization. It
does not name which GPU, display-controller, driver, or scheduling stage
caused it. See
[TransactionCompletedThread.cpp](https://android.googlesource.com/platform/frameworks/native/+/dd81befb570dfda0e20b5c9a6c233ab27f1c3d93/services/surfaceflinger/TransactionCompletedThread.cpp)
and the
[present-fence contract](https://android.googlesource.com/platform/hardware/interfaces/+/master/graphics/composer/2.1/IComposerClient.hal).

## What Flutter actually configures

The installed SDK reports Flutter 3.47.8, framework revision
`737ba8f885`. Its local embedding source was inspected, and the matching
upstream revision is linked below.

1. `FlutterActivity.getRenderMode()` defaults to `surface` for an opaque
   Activity background and `texture` for a transparent background.
   `getTransparencyMode()` independently follows that background choice.
   Changing only the render mode does not change transparency. The
   `onFlutterSurfaceViewCreated()` callback is a customization hook.
   [FlutterActivity.java](https://github.com/flutter/flutter/blob/737ba8f885/engine/src/flutter/shell/platform/android/io/flutter/embedding/android/FlutterActivity.java)
2. The delegate constructs `FlutterSurfaceView(context, transparent)`
   from those two settings, calls the customization hook, then wraps it
   in FlutterView. On the texture branch it explicitly sets the
   FlutterTextureView's opacity from `getTransparencyMode()`.
   [FlutterActivityAndFragmentDelegate.java](https://github.com/flutter/flutter/blob/737ba8f885/engine/src/flutter/shell/platform/android/io/flutter/embedding/android/FlutterActivityAndFragmentDelegate.java)
3. `FlutterSurfaceView(true)` requests `PixelFormat.TRANSPARENT` and
   calls `setZOrderOnTop(true)`. Thus a transparent surface-mode UI is
   supported. Its `gatherTransparentRegion()` override also removes its
   entire rectangle from the window's transparent-region hint when its
   view alpha is 1, as a TalkBack workaround. Pixel transparency and
   that optimization hint are distinct.
   [FlutterSurfaceView.java](https://github.com/flutter/flutter/blob/737ba8f885/engine/src/flutter/shell/platform/android/io/flutter/embedding/android/FlutterSurfaceView.java)

The current uncommitted example overrides only `getRenderMode()` to
return `texture`. Unless another setting changes the Activity background,
the delegate therefore initially configures an opaque FlutterTextureView.
The new probe explicitly tested both opaque and transparent TextureView
hosts; neither recovered hardware video composition.

The plugin's current `LycheeSurfaceView.create()` accepts only a
FlutterView containing a FlutterTextureView. Merely changing the Activity
to surface mode makes this check reject the native path and the player
fall back to a Flutter texture. A successful video after that one-line
change would be an invalid test of the proposed arrangement.

## What changes with a transparent Flutter surface

The current route makes Android draw Flutter's TextureView into the app
window. A SurfaceView instead supplies a separate compositor layer.
HWC can use that layer as an overlay when the device supports the
configuration. This distinction is documented in
[AOSP's TextureView architecture](https://source.android.com/docs/core/graphics/arch-tv).

The intended surface arrangement is, from front to back:

```text
+--------------------------------------------+
| Transparent FlutterSurfaceView              |
| controls / subtitles; clear video rectangle |
+--------------------------------------------+
| Activity window                             |
| video SurfaceView punches a hole here        |
+--------------------------------------------+
| Native video SurfaceView                     |
| MediaCodec buffers                           |
+--------------------------------------------+
| Display                                      |
+--------------------------------------------+
```

Android's default SurfaceView is below its window and clears a hole in
that window. `setZOrderOnTop(true)` places a surface above the window;
`setZOrderMediaOverlay(true)` places it above a regular SurfaceView but
still below the window. View-tree sibling order alone does not express
these compositor relationships. The separate Activity-window layer must
also be inspected, since it can sit between video and Flutter. See the
[SurfaceView API](https://developer.android.com/reference/android/view/SurfaceView)
and
[AOSP SurfaceView's hole-punch implementation](https://github.com/aosp-mirror/platform_frameworks_base/blob/android-12.0.0_r1/core/java/android/view/SurfaceView.java).

An absent transparent-region hint cannot rule out all transparency or
occlusion problems. A hint is an optimization; actual window pixels,
opacity flags, and surface order still matter. Do not change the hint
implementation as a presumed fix without a separate experiment.

## Why keep investigating SurfaceView

The texture path's low drop count is useful as a timing baseline, but
does not establish equivalent image quality. Android recommends
SurfaceView for video because it supports more accurate frame timing,
higher-quality HDR on capable devices, and full panel resolution on TVs
whose UI is rendered at a lower resolution. Thus routing everything
through the Flutter texture can avoid this symptom while losing a reason
the native route exists.
[Android's surface selection guide](https://developer.android.com/media/media3/ui/surface)

Avoiding Hybrid Composition is also a reasonable experiment. Flutter's
documentation says it merges the raster thread into the platform thread,
which makes rasterization compete with platform messages and plugins.
That explains a possible source of missed callbacks in the older path,
but does not prove it accounts for that TV's missing frames.
[Flutter Platform Views performance](https://docs.flutter.dev/platform-integration/android/platform-views#performance)

## Foreground graphics trace

A five-second atrace capture (`gfx view video`, 16 MiB buffer) was taken
with the static transparent-SurfaceView page. A SurfaceFlinger sample
inside the trace confirmed that the player's native 4K video layer and
both upper layers were visible and CLIENT. Flutter remained at five
startup frames. Two earlier trace attempts were excluded because the
TV's dream/screen-saver layer was active; their smooth background
statistics were not measurements of visible video.

On the lychee-vsync thread the foreground trace contains 267 complete
Choreographer#doFrame slices over 4.984 seconds between first and last
starts: approximately 53.4 callbacks/s, with 33 inter-callback gaps over
25 ms and a maximum gap of 36.9 ms. The callback duration was 1.29 ms
median and 13.23 ms maximum; none exceeded 16 ms. The full run reported
423 rendered, 24 dropped, position 10.230 s. Thus the native frame driver
still misses callbacks in this configuration even though Flutter is no
longer repainting. It is not spending an entire refresh period inside
any observed callback.

The same trace contains 215 HEVC CCodecBufferChannel::onWorkDone slices
and 201 app queueBuffer slices. These are trace events, not public-player
frame counters, and do not alone establish decoder throughput or the
cause of buffer retention. They nevertheless show why missing display
callbacks alone is not yet an adequate explanation for the roughly
42-frame/s acquisition count. No long app-side dequeueBuffer wait was
observed (maximum 2.95 ms), so this trace does not support a simple
single long dequeue stall either.

A matching five-second capture of the installed native SDK example
confirmed DEVICE(MM): 299 complete frame callbacks over 4.983 seconds,
about 59.8 callbacks/s, with four gaps over 25 ms, and 300 HEVC work-done
slices. Its callback median was 1.86 ms and maximum 14.81 ms, so callback
body execution was not faster than Flutter's failing configuration.
The native full run reported 623 rendered, 4 dropped, position 10.462 s.
These instrumented runs are additional diagnostics and are excluded from
the uninstrumented table above. The installed SDK APK and video bounds
still differ from the Flutter probe, so the comparison localizes the
symptom without proving a particular cause. Native trace evidence is in
`/tmp/lychee-tv-research/trace-sdk/`.

Evidence: `/tmp/lychee-tv-research/trace-foreground/`, including process
thread IDs, `surface.atrace.txt`, contemporaneous `sf.txt`, and process
logs. The parser is `/tmp/lychee-tv-research/trace/analyze.py`.

## Remaining investigation

The transparent-surface and static-page experiments above are complete.
Do not repeat them as presumed fixes. The foreground trace narrows the
symptom but does not explain the vendor's hardware-plane rejection or
slower frame output. A useful next probe must correlate composer
validation/rejection evidence with decoder output-buffer ownership,
release callbacks and actual presentation times. Instrument those
boundaries, rather than changing more view settings without a prediction.

Keep the decoder, media, video bounds and display mode fixed. Inspect all
three layers when testing surface mode. If changing a single scene
property restores DEVICE(MM), verify the delivery/presentation cadence
before retaining a patch. A blank page, missing native video layer or
screen-saver layer invalidates the trial.

The hardware composer is allowed to choose how it splits composition
work; Android's implementation guidance explicitly includes policies
based on the current scene and its updates. There is no public
SurfaceView setting that guarantees the desired hardware plane.
[HWC implementation guidance](https://source.android.com/docs/core/graphics/implement-hwc)
