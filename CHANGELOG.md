## 0.3.4

* always enable audio resample.

## 0.3.3

* fix set playWhenReady to false in initialized state may cause wrong player state.
* support set/get volume.

## 0.3.2

* fix video stream is attached pic make player state can not transfer to ready state.
* add a workaround for message process maybe blocked.
* fix video decode thread de-construct may run in dead loop.

## 0.3.1

* restrict ffmpeg-kit-macos-full to 4.4 on macOS
* publish ffplay directory to pub

## 0.3.0

* basic audio play support on Windows, Linux, macOS.
