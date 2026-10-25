// JNI for tech.soit.lychee: LycheePlayer over the C ABI, and
// LycheeFrameDriver, which shows a player's frames in a Surface.
//
// Strings cross as UTF-8 byte arrays: NewStringUTF and GetStringUTFChars
// speak modified UTF-8, which breaks on characters outside the BMP.

#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "lychee/lychee.h"

namespace {

JavaVM* g_vm = nullptr;
// LycheePlayer.EventSink.onEvent(int, long, long).
jmethodID g_on_event = nullptr;

lychee_player* Player(jlong handle) {
  return reinterpret_cast<lychee_player*>(handle);
}

// On the player's threads.
void OnEvent(void* sink, int32_t type, int64_t arg0, int64_t arg1) {
  JNIEnv* env = nullptr;
  bool attached = false;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) ==
      JNI_EDETACHED) {
    // Events are rare; attaching for each one keeps no thread attached when
    // the player's threads exit.
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
      return;
    }
    attached = true;
  }
  env->CallVoidMethod(static_cast<jobject>(sink), g_on_event, type,
                      static_cast<jlong>(arg0), static_cast<jlong>(arg1));
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  if (attached) {
    g_vm->DetachCurrentThread();
  }
}

jbyteArray ToBytes(JNIEnv* env, const char* data, size_t size) {
  jbyteArray array = env->NewByteArray(static_cast<jsize>(size));
  env->SetByteArrayRegion(array, 0, static_cast<jsize>(size),
                          reinterpret_cast<const jbyte*>(data));
  return array;
}

// UTF-8 bytes from Kotlin.
std::string FromBytes(JNIEnv* env, jbyteArray bytes) {
  std::string result(static_cast<size_t>(env->GetArrayLength(bytes)), '\0');
  env->GetByteArrayRegion(bytes, 0, static_cast<jsize>(result.size()),
                          reinterpret_cast<jbyte*>(result.data()));
  return result;
}

// Calls |copy(buffer, size)|, which returns the full length (<= 0: none),
// again with a larger buffer if the first was too small.
template <typename Copy>
jbyteArray ReadBytes(JNIEnv* env, Copy copy) {
  std::vector<char> buffer(256);
  while (true) {
    const int32_t length =
        copy(buffer.data(), static_cast<int32_t>(buffer.size()));
    if (length <= 0) {
      return nullptr;
    }
    if (static_cast<size_t>(length) < buffer.size()) {
      return ToBytes(env, buffer.data(), static_cast<size_t>(length));
    }
    buffer.resize(static_cast<size_t>(length) + 1);
  }
}

// A NUL-terminated char array of a C struct.
template <size_t N>
std::string FromArray(const char (&array)[N]) {
  return std::string(array, strnlen(array, N));
}

// The frame driver's state, shared by its vsync thread (Render) and the main
// thread (the setters, Detach); the mutex keeps them apart, so after
// Detach() returns the player is no longer used.
//
// Hardware-decoded frames (LYCHEE_FRAME_ANDROID_SURFACE) are rendered by
// MediaCodec straight into the window given to the player with
// lychee_player_set_video_surface(); acquiring a frame releases it there.
// Software-decoded frames (LYCHEE_FRAME_CPU_RGBA) are copied into another
// window: a window a CPU has drawn into stays connected to the CPU, and
// MediaCodec can no longer render into it.
struct Driver {
  std::mutex mutex;
  lychee_player* player = nullptr;
  // Rotate CPU frames as MediaCodec rotates surface frames for a player
  // created with rotate_surface_frames.
  bool upright = false;
  // Where CPU frames are drawn, and its size when it was set.
  ANativeWindow* cpu_window = nullptr;
  int32_t cpu_window_width = 0;
  int32_t cpu_window_height = 0;
  // Draw the current frame again even if the player has no new one: a new
  // window starts out empty.
  bool redraw = false;
  int32_t frame_type = 0;
  int32_t frame_width = 0;
  int32_t frame_height = 0;
};

Driver* FromHandle(jlong handle) {
  return reinterpret_cast<Driver*>(handle);
}

void ReleaseCpuWindowLocked(Driver* driver) {
  if (driver->cpu_window) {
    ANativeWindow_release(driver->cpu_window);
    driver->cpu_window = nullptr;
  }
}

int32_t Transform(int32_t rotation) {
  switch (rotation) {
    case 90:
      return ANATIVEWINDOW_TRANSFORM_ROTATE_90;
    case 180:
      return ANATIVEWINDOW_TRANSFORM_ROTATE_180;
    case 270:
      return ANATIVEWINDOW_TRANSFORM_ROTATE_270;
    default:
      return ANATIVEWINDOW_TRANSFORM_IDENTITY;
  }
}

// The rotation CPU frames are drawn with.
int32_t RotationLocked(Driver* driver) {
  return driver->upright ? lychee_player_get_video_rotation(driver->player) : 0;
}

// The window size a CPU frame needs: rotated, since a SurfaceView drops
// buffers whose size, after their transform, is not the window's.
void WindowSizeLocked(Driver* driver,
                      int32_t width,
                      int32_t height,
                      int32_t* window_width,
                      int32_t* window_height) {
  const bool sideways = RotationLocked(driver) % 180 != 0;
  *window_width = sideways ? height : width;
  *window_height = sideways ? width : height;
}

// Returns false if the window is not the frame's size yet; the caller
// gets a new window of that size.
bool DrawLocked(Driver* driver, lychee_frame* frame) {
  const int32_t width = lychee_frame_get_width(frame);
  const int32_t height = lychee_frame_get_height(frame);
  int32_t window_width = 0;
  int32_t window_height = 0;
  WindowSizeLocked(driver, width, height, &window_width, &window_height);
  ANativeWindow* window = driver->cpu_window;
  // The size it had when set: ANativeWindow_getWidth() reports the buffer
  // size once it is set below.
  if (!window || driver->cpu_window_width != window_width ||
      driver->cpu_window_height != window_height) {
    return false;
  }
  ANativeWindow_setBuffersGeometry(window, width, height,
                                   WINDOW_FORMAT_RGBA_8888);
  ANativeWindow_setBuffersTransform(window, Transform(RotationLocked(driver)));
  ANativeWindow_Buffer buffer;
  if (ANativeWindow_lock(window, &buffer, nullptr) != 0) {
    return false;
  }
  int32_t stride = 0;
  const uint8_t* source = lychee_frame_get_data(frame, &stride);
  if (source) {
    auto* dest = static_cast<uint8_t*>(buffer.bits);
    const size_t row = static_cast<size_t>(std::min(width, buffer.width)) * 4;
    const int32_t rows = std::min(height, buffer.height);
    for (int32_t y = 0; y < rows; ++y) {
      std::memcpy(dest + static_cast<size_t>(y) * buffer.stride * 4,
                  source + static_cast<size_t>(y) * stride, row);
    }
  }
  ANativeWindow_unlockAndPost(window);
  return true;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  g_vm = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  jclass sink = env->FindClass("tech/soit/lychee/LycheePlayer$EventSink");
  if (!sink) {
    return JNI_ERR;
  }
  g_on_event = env->GetMethodID(sink, "onEvent", "(IJJ)V");
  env->DeleteLocalRef(sink);
  return g_on_event ? JNI_VERSION_1_6 : JNI_ERR;
}

// LycheePlayer.

JNIEXPORT jlong JNICALL Java_tech_soit_lychee_LycheePlayer_nativeCreate(
    JNIEnv*,
    jclass,
    jboolean hardware_decoding,
    jboolean video) {
  lychee_config config;
  lychee_config_init(&config);
  config.video_frame_types =
      video ? LYCHEE_FRAME_ANDROID_SURFACE | LYCHEE_FRAME_CPU_RGBA : 0;
  config.hardware_decoding = hardware_decoding;
  config.rotate_surface_frames = 1;
  return reinterpret_cast<jlong>(lychee_player_create(&config));
}

// Returns the global reference |sink| is held by, for nativeDestroy().
JNIEXPORT jlong JNICALL Java_tech_soit_lychee_LycheePlayer_nativeSetEventSink(
    JNIEnv* env,
    jclass,
    jlong handle,
    jobject sink) {
  jobject ref = env->NewGlobalRef(sink);
  lychee_player_set_event_callback(Player(handle), &OnEvent, ref);
  return reinterpret_cast<jlong>(ref);
}

JNIEXPORT void JNICALL Java_tech_soit_lychee_LycheePlayer_nativeDestroy(
    JNIEnv* env,
    jclass,
    jlong handle,
    jlong sink) {
  // Joins the player's threads: no event follows.
  lychee_player_destroy(Player(handle));
  env->DeleteGlobalRef(reinterpret_cast<jobject>(sink));
}

JNIEXPORT jint JNICALL Java_tech_soit_lychee_LycheePlayer_nativeOpen(
    JNIEnv* env,
    jclass,
    jlong handle,
    jbyteArray url) {
  return lychee_player_open(Player(handle), FromBytes(env, url).c_str());
}

JNIEXPORT void JNICALL Java_tech_soit_lychee_LycheePlayer_nativePlay(JNIEnv*,
                                                                     jclass,
                                                                     jlong handle) {
  lychee_player_play(Player(handle));
}

JNIEXPORT void JNICALL Java_tech_soit_lychee_LycheePlayer_nativePause(JNIEnv*,
                                                                      jclass,
                                                                      jlong handle) {
  lychee_player_pause(Player(handle));
}

JNIEXPORT void JNICALL Java_tech_soit_lychee_LycheePlayer_nativeSeek(JNIEnv*,
                                                                     jclass,
                                                                     jlong handle,
                                                                     jlong position_us) {
  lychee_player_seek(Player(handle), position_us);
}

JNIEXPORT void JNICALL Java_tech_soit_lychee_LycheePlayer_nativeSetVolume(
    JNIEnv*,
    jclass,
    jlong handle,
    jfloat volume) {
  lychee_player_set_volume(Player(handle), volume);
}

JNIEXPORT void JNICALL Java_tech_soit_lychee_LycheePlayer_nativeSetRate(JNIEnv*,
                                                                        jclass,
                                                                        jlong handle,
                                                                        jdouble rate) {
  lychee_player_set_rate(Player(handle), rate);
}

JNIEXPORT jdouble JNICALL Java_tech_soit_lychee_LycheePlayer_nativeGetRate(
    JNIEnv*,
    jclass,
    jlong handle) {
  return lychee_player_get_rate(Player(handle));
}

JNIEXPORT void JNICALL Java_tech_soit_lychee_LycheePlayer_nativeSetLooping(
    JNIEnv*,
    jclass,
    jlong handle,
    jboolean looping) {
  lychee_player_set_looping(Player(handle), looping);
}

JNIEXPORT jboolean JNICALL Java_tech_soit_lychee_LycheePlayer_nativeIsLooping(
    JNIEnv*,
    jclass,
    jlong handle) {
  return lychee_player_is_looping(Player(handle)) != 0;
}

JNIEXPORT jint JNICALL Java_tech_soit_lychee_LycheePlayer_nativeGetState(
    JNIEnv*,
    jclass,
    jlong handle) {
  return lychee_player_get_state(Player(handle));
}

JNIEXPORT jboolean JNICALL Java_tech_soit_lychee_LycheePlayer_nativeIsPlaying(
    JNIEnv*,
    jclass,
    jlong handle) {
  return lychee_player_is_playing(Player(handle)) != 0;
}

JNIEXPORT jlong JNICALL Java_tech_soit_lychee_LycheePlayer_nativeGetPosition(
    JNIEnv*,
    jclass,
    jlong handle) {
  return lychee_player_get_position(Player(handle));
}

JNIEXPORT jlong JNICALL Java_tech_soit_lychee_LycheePlayer_nativeGetDuration(
    JNIEnv*,
    jclass,
    jlong handle) {
  return lychee_player_get_duration(Player(handle));
}

JNIEXPORT jlong JNICALL
Java_tech_soit_lychee_LycheePlayer_nativeGetBufferedPosition(JNIEnv*,
                                                             jclass,
                                                             jlong handle) {
  return lychee_player_get_buffered_position(Player(handle));
}

// width << 32 | height, or 0 without video.
JNIEXPORT jlong JNICALL Java_tech_soit_lychee_LycheePlayer_nativeGetVideoSize(
    JNIEnv*,
    jclass,
    jlong handle) {
  int32_t width = 0;
  int32_t height = 0;
  if (!lychee_player_get_video_size(Player(handle), &width, &height)) {
    return 0;
  }
  return (static_cast<jlong>(width) << 32) | static_cast<uint32_t>(height);
}

JNIEXPORT jint JNICALL Java_tech_soit_lychee_LycheePlayer_nativeGetRotation(
    JNIEnv*,
    jclass,
    jlong handle) {
  return lychee_player_get_video_rotation(Player(handle));
}

JNIEXPORT jbyteArray JNICALL Java_tech_soit_lychee_LycheePlayer_nativeGetError(
    JNIEnv* env,
    jclass,
    jlong handle) {
  return ReadBytes(env, [handle](char* buffer, int32_t size) {
    return lychee_player_get_error(Player(handle), buffer, size);
  });
}

JNIEXPORT jint JNICALL Java_tech_soit_lychee_LycheePlayer_nativeGetTrackCount(
    JNIEnv*,
    jclass,
    jlong handle) {
  return lychee_player_get_track_count(Player(handle));
}

// Fills |ints| with type, id, selected, width, height, sample_rate and
// channels, and returns codec, language and title separated by NULs; null
// if |index| is out of range.
JNIEXPORT jbyteArray JNICALL
Java_tech_soit_lychee_LycheePlayer_nativeGetTrackInfo(JNIEnv* env,
                                                      jclass,
                                                      jlong handle,
                                                      jint index,
                                                      jintArray ints) {
  lychee_track_info info{};
  info.struct_size = sizeof(info);
  if (lychee_player_get_track_info(Player(handle), index, &info) != 0) {
    return nullptr;
  }
  const jint values[] = {info.type,   info.id,          info.selected,
                         info.width,  info.height,      info.sample_rate,
                         info.channels};
  env->SetIntArrayRegion(ints, 0, 7, values);
  const std::string strings = FromArray(info.codec) + '\0' +
                              FromArray(info.language) + '\0' +
                              FromArray(info.title);
  return ToBytes(env, strings.data(), strings.size());
}

JNIEXPORT jboolean JNICALL Java_tech_soit_lychee_LycheePlayer_nativeSelectTrack(
    JNIEnv*,
    jclass,
    jlong handle,
    jint type,
    jint index) {
  return lychee_player_select_track(Player(handle), type, index) == 0;
}

JNIEXPORT jbyteArray JNICALL
Java_tech_soit_lychee_LycheePlayer_nativeGetSubtitle(JNIEnv* env,
                                                     jclass,
                                                     jlong handle) {
  return ReadBytes(env, [handle](char* buffer, int32_t size) {
    return lychee_player_get_subtitle(Player(handle), 0, buffer, size);
  });
}

JNIEXPORT jbyteArray JNICALL
Java_tech_soit_lychee_LycheePlayer_nativeGetMetadata(JNIEnv* env,
                                                     jclass,
                                                     jlong handle,
                                                     jbyteArray key) {
  const std::string name = FromBytes(env, key);
  return ReadBytes(env, [handle, &name](char* buffer, int32_t size) {
    return lychee_player_get_metadata(Player(handle), name.c_str(), buffer,
                                      size);
  });
}

JNIEXPORT jbyteArray JNICALL
Java_tech_soit_lychee_LycheePlayer_nativeGetArtwork(JNIEnv* env,
                                                    jclass,
                                                    jlong handle) {
  return ReadBytes(env, [handle](char* buffer, int32_t size) {
    return lychee_player_get_artwork(
        Player(handle), reinterpret_cast<uint8_t*>(buffer), size);
  });
}

JNIEXPORT jlong JNICALL
Java_tech_soit_lychee_LycheePlayer_nativeAcquireSubtitle(JNIEnv*,
                                                         jclass,
                                                         jlong handle,
                                                         jint width,
                                                         jint height) {
  return reinterpret_cast<jlong>(
      lychee_player_acquire_subtitle(Player(handle), 0, width, height));
}

JNIEXPORT void JNICALL
Java_tech_soit_lychee_LycheePlayer_nativeSubtitleRelease(JNIEnv*,
                                                         jclass,
                                                         jlong subtitle) {
  lychee_subtitle_release(reinterpret_cast<lychee_subtitle*>(subtitle));
}

JNIEXPORT jlong JNICALL
Java_tech_soit_lychee_LycheePlayer_nativeSubtitleId(JNIEnv*,
                                                    jclass,
                                                    jlong subtitle) {
  return lychee_subtitle_get_id(reinterpret_cast<lychee_subtitle*>(subtitle));
}

JNIEXPORT jint JNICALL
Java_tech_soit_lychee_LycheePlayer_nativeSubtitleImageCount(JNIEnv*,
                                                            jclass,
                                                            jlong subtitle) {
  return lychee_subtitle_get_image_count(
      reinterpret_cast<lychee_subtitle*>(subtitle));
}

// Fills |ints| with x, y, width, height, bitmap_width and bitmap_height,
// and returns the pixels, rows of bitmap_width * 4 bytes.
JNIEXPORT jbyteArray JNICALL
Java_tech_soit_lychee_LycheePlayer_nativeSubtitleImage(JNIEnv* env,
                                                       jclass,
                                                       jlong subtitle,
                                                       jint index,
                                                       jintArray ints) {
  lychee_subtitle_image image{};
  image.struct_size = sizeof(image);
  if (lychee_subtitle_get_image(reinterpret_cast<lychee_subtitle*>(subtitle),
                                index, &image) != 0) {
    return nullptr;
  }
  const jint values[] = {image.x,     image.y,
                         image.width, image.height,
                         image.bitmap_width, image.bitmap_height};
  env->SetIntArrayRegion(ints, 0, 6, values);
  const size_t row = static_cast<size_t>(image.bitmap_width) * 4;
  std::vector<char> pixels(row * image.bitmap_height);
  for (int32_t y = 0; y < image.bitmap_height; ++y) {
    std::memcpy(pixels.data() + row * y,
                image.data + static_cast<size_t>(image.stride) * y, row);
  }
  return ToBytes(env, pixels.data(), pixels.size());
}

// Fills |longs| with hardware_decoding, frames_rendered, frames_dropped,
// video_buffered_us, audio_buffered_us and video_decode_time_us, and
// returns the video decoder's name.
JNIEXPORT jbyteArray JNICALL Java_tech_soit_lychee_LycheePlayer_nativeGetStats(
    JNIEnv* env,
    jclass,
    jlong handle,
    jlongArray longs) {
  lychee_stats stats{};
  stats.struct_size = sizeof(stats);
  lychee_player_get_stats(Player(handle), &stats);
  const jlong values[] = {stats.hardware_decoding,  stats.frames_rendered,
                          stats.frames_dropped,     stats.video_buffered_us,
                          stats.audio_buffered_us,  stats.video_decode_time_us};
  env->SetLongArrayRegion(longs, 0, 6, values);
  const std::string decoder = FromArray(stats.video_decoder);
  return ToBytes(env, decoder.data(), decoder.size());
}

// LycheeFrameDriver.

JNIEXPORT jlong JNICALL Java_tech_soit_lychee_LycheeFrameDriver_nativeCreate(
    JNIEnv*,
    jclass,
    jlong player,
    jboolean upright) {
  auto* driver = new Driver();
  driver->player = Player(player);
  driver->upright = upright;
  return reinterpret_cast<jlong>(driver);
}

// The window MediaCodec renders into; null detaches it.
JNIEXPORT void JNICALL Java_tech_soit_lychee_LycheeFrameDriver_nativeSetOutput(
    JNIEnv* env,
    jclass,
    jlong handle,
    jobject surface) {
  Driver* driver = FromHandle(handle);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (!driver->player) {
    return;
  }
  ANativeWindow* window =
      surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
  // The player takes its own reference.
  lychee_player_set_video_surface(driver->player, window);
  if (window) {
    ANativeWindow_release(window);
  }
}

// The window CPU frames are copied into; null detaches it.
JNIEXPORT void JNICALL
Java_tech_soit_lychee_LycheeFrameDriver_nativeSetCpuSurface(JNIEnv* env,
                                                            jclass,
                                                            jlong handle,
                                                            jobject surface) {
  Driver* driver = FromHandle(handle);
  std::lock_guard<std::mutex> lock(driver->mutex);
  ReleaseCpuWindowLocked(driver);
  if (surface && driver->player) {
    driver->cpu_window = ANativeWindow_fromSurface(env, surface);
    driver->cpu_window_width = ANativeWindow_getWidth(driver->cpu_window);
    driver->cpu_window_height = ANativeWindow_getHeight(driver->cpu_window);
  }
  driver->redraw = true;
}

// Returns the latest frame's type and the size a CPU window for it needs
// (the frame's size, rotated for an upright driver) as
// (type << 48 | width << 24 | height), 0 before the first frame.
JNIEXPORT jlong JNICALL Java_tech_soit_lychee_LycheeFrameDriver_nativeRender(
    JNIEnv*,
    jclass,
    jlong handle,
    jlong display_time_us) {
  Driver* driver = FromHandle(handle);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (!driver->player) {
    return 0;
  }
  // Called every vsync, drawn or not: the player estimates the display's
  // refresh interval from these calls.
  const bool changed =
      lychee_player_has_new_frame(driver->player, display_time_us) != 0;
  if (changed || driver->redraw) {
    // Acquiring a surface frame is what shows it.
    lychee_frame* frame =
        lychee_player_acquire_frame(driver->player, display_time_us);
    if (frame) {
      driver->frame_type = lychee_frame_get_type(frame);
      WindowSizeLocked(driver, lychee_frame_get_width(frame),
                       lychee_frame_get_height(frame), &driver->frame_width,
                       &driver->frame_height);
      driver->redraw = driver->frame_type == LYCHEE_FRAME_CPU_RGBA &&
                       !DrawLocked(driver, frame);
      lychee_frame_release(frame);
    }
  }
  return (static_cast<jlong>(driver->frame_type) << 48) |
         (static_cast<jlong>(driver->frame_width & 0xffffff) << 24) |
         (driver->frame_height & 0xffffff);
}

// After this returns, the player is not touched again.
JNIEXPORT void JNICALL Java_tech_soit_lychee_LycheeFrameDriver_nativeDetach(
    JNIEnv*,
    jclass,
    jlong handle) {
  Driver* driver = FromHandle(handle);
  std::lock_guard<std::mutex> lock(driver->mutex);
  if (driver->player) {
    lychee_player_set_video_surface(driver->player, nullptr);
  }
  driver->player = nullptr;
  ReleaseCpuWindowLocked(driver);
}

JNIEXPORT void JNICALL Java_tech_soit_lychee_LycheeFrameDriver_nativeDestroy(
    JNIEnv*,
    jclass,
    jlong handle) {
  delete FromHandle(handle);
}

}  // extern "C"
