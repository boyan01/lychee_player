# ffplayer

player wrapper for ffmpeg.

## compile & Run

### Windows 10

* if you use Clion as your IDEA, you should config toolchains architecture to `amd64`

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

1. Install ffmpeg.

```shell
brew install ffmpeg
```

3. Open Project(`ffplayer`) in Clion/Vscode.