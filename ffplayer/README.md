# ffplayer

player wrapper for ffmpeg.

## compile & Run

### Windows 10

1. install vcpkg. https://github.com/microsoft/vcpkg#quick-start-windows
2. vcpkg install `ffmpeg` and `sdl2`.

```shell
vcpkg install ffmpeg
vcpkg install sdl2:x64-windows
```

* clion should config architecture to `x86_amd64`

### Linux (Ubuntu 20.04)

1. Install av libs.

```shell
sudo apt install libavcodec-dev libavformat-dev libavdevice-dev
```

2. Install sdl2

```shell
sudo apt-get install libsdl2-dev
```

3. Open the project (`audio_player/ffplayer`) in clion/vscode, click **run**.

### Macos

1. Install av libs

```shell
brew install ffmpeg
```

2. Install SDL2

```shell
brew install SDL2
```

3. Open Project(`ffplayer`) in Clion/Vscode.