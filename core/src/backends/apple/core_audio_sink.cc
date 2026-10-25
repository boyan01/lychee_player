#define LOG_TAG "core_audio"

#include "backends/apple/core_audio_sink.h"

#include <Block.h>

#include "backends/apple/audio_session.h"

#include <cstring>

#include "base/log.h"
#include "base/time.h"

namespace lychee {

namespace {

constexpr int kChannels = 2;

#if TARGET_OS_OSX
UInt32 GetDeviceProperty(AudioDeviceID device, AudioObjectPropertySelector selector) {
  AudioObjectPropertyAddress address = {selector, kAudioObjectPropertyScopeOutput,
                                        kAudioObjectPropertyElementMain};
  UInt32 value = 0;
  UInt32 size = sizeof(value);
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) !=
      noErr) {
    return 0;
  }
  return value;
}

UInt32 GetStreamLatency(AudioDeviceID device) {
  AudioObjectPropertyAddress address = {kAudioDevicePropertyStreams,
                                        kAudioObjectPropertyScopeOutput,
                                        kAudioObjectPropertyElementMain};
  AudioStreamID stream = 0;
  UInt32 size = sizeof(stream);
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size,
                                 &stream) != noErr ||
      size < sizeof(stream)) {
    return 0;
  }
  address = {kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal,
             kAudioObjectPropertyElementMain};
  UInt32 value = 0;
  size = sizeof(value);
  if (AudioObjectGetPropertyData(stream, &address, 0, nullptr, &size,
                                 &value) != noErr) {
    return 0;
  }
  return value;
}

Float64 GetNominalSampleRate(AudioDeviceID device) {
  AudioObjectPropertyAddress address = {kAudioDevicePropertyNominalSampleRate,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain};
  Float64 value = 0;
  UInt32 size = sizeof(value);
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size,
                                 &value) != noErr) {
    return 0;
  }
  return value;
}

AudioDeviceID GetDefaultOutputDevice() {
  AudioObjectPropertyAddress address = {
      kAudioHardwarePropertyDefaultOutputDevice,
      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  AudioDeviceID device = kAudioObjectUnknown;
  UInt32 size = sizeof(device);
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                             &size, &device);
  return device;
}

constexpr AudioObjectPropertyAddress kDefaultOutputDeviceAddress = {
    kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain};
#endif

}  // namespace

CoreAudioSink::CoreAudioSink() = default;

CoreAudioSink::~CoreAudioSink() {
#if TARGET_OS_IOS
  if (session_observer_) {
    RemoveAudioSessionObserver(session_observer_);
  }
#endif
#if TARGET_OS_OSX
  if (listener_) {
    AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject,
                                           &kDefaultOutputDeviceAddress,
                                           listener_queue_, listener_);
    // A notification already dispatched may still run; let it finish.
    dispatch_sync(listener_queue_, ^{
                  });
    Block_release(listener_);
  }
  if (listener_queue_) {
    dispatch_release(listener_queue_);
  }
#endif
  if (unit_) {
    Stop();
    AudioUnitUninitialize(unit_);
    AudioComponentInstanceDispose(unit_);
  }
}

bool CoreAudioSink::Open(RenderCallback callback, std::string* error) {
  callback_ = std::move(callback);
  AudioComponentDescription description = {};
  description.componentType = kAudioUnitType_Output;
#if TARGET_OS_OSX
  description.componentSubType = kAudioUnitSubType_DefaultOutput;
#else
  description.componentSubType = kAudioUnitSubType_RemoteIO;
#endif
  description.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = AudioComponentFindNext(nullptr, &description);
  if (!component || AudioComponentInstanceNew(component, &unit_) != noErr) {
    *error = "no audio output unit";
    unit_ = nullptr;
    return false;
  }

  // Render at the device rate so the AudioUnit does not resample.
  AudioStreamBasicDescription device_format = {};
  UInt32 size = sizeof(device_format);
  AudioUnitGetProperty(unit_, kAudioUnitProperty_StreamFormat,
                       kAudioUnitScope_Output, 0, &device_format, &size);
  int sample_rate = device_format.mSampleRate > 0
                        ? static_cast<int>(device_format.mSampleRate)
                        : 48000;

  AudioStreamBasicDescription stream_format = {};
  stream_format.mSampleRate = sample_rate;
  stream_format.mFormatID = kAudioFormatLinearPCM;
  stream_format.mFormatFlags =
      kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  stream_format.mChannelsPerFrame = kChannels;
  stream_format.mBitsPerChannel = 32;
  stream_format.mFramesPerPacket = 1;
  stream_format.mBytesPerFrame = sizeof(float) * kChannels;
  stream_format.mBytesPerPacket = stream_format.mBytesPerFrame;
  OSStatus status = AudioUnitSetProperty(
      unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
      &stream_format, sizeof(stream_format));
  if (status != noErr) {
    *error = "set stream format failed: " + std::to_string(status);
    return false;
  }

  AURenderCallbackStruct render = {&OnRender, this};
  status = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback,
                                kAudioUnitScope_Input, 0, &render,
                                sizeof(render));
  if (status == noErr) {
    status = AudioUnitInitialize(unit_);
  }
  if (status != noErr) {
    *error = "AudioUnitInitialize failed: " + std::to_string(status);
    return false;
  }
  format_ = {sample_rate, kChannels};
  output_latency_ = QueryOutputLatency();
  LOGI("opened: %d Hz, %d ch, latency %lld us", sample_rate, kChannels,
       static_cast<long long>(output_latency_.load()));
#if TARGET_OS_OSX
  // Headphones plugged in, AirPods connected, a device picked in Control
  // Center: the unit moves to the new device, whose latency may differ by
  // a hundred milliseconds or more.
  listener_queue_ =
      dispatch_queue_create("lychee.core_audio", DISPATCH_QUEUE_SERIAL);
  listener_ = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) {
    UpdateOutputLatency();
  });
  if (AudioObjectAddPropertyListenerBlock(
          kAudioObjectSystemObject, &kDefaultOutputDeviceAddress,
          listener_queue_, listener_) != noErr) {
    LOGW("cannot follow default output device changes");
    Block_release(listener_);
    listener_ = nullptr;
  }
#elif TARGET_OS_IOS
  session_observer_ = AddAudioSessionObserver([this](AudioSessionEvent event) {
    if (event == AudioSessionEvent::kRouteChanged) {
      UpdateOutputLatency();
    } else if (started_) {
      // The system stopped the unit for the interruption.
      AudioOutputUnitStart(unit_);
    }
  });
#endif
  return true;
}

void CoreAudioSink::UpdateOutputLatency() {
  int64_t latency = QueryOutputLatency();
  int64_t previous = output_latency_.exchange(latency);
  if (latency != previous) {
    LOGI("output device changed: latency %lld -> %lld us",
         static_cast<long long>(previous), static_cast<long long>(latency));
  }
}

int64_t CoreAudioSink::QueryOutputLatency() const {
  Float64 unit_latency = 0;
  UInt32 size = sizeof(unit_latency);
  AudioUnitGetProperty(unit_, kAudioUnitProperty_Latency,
                       kAudioUnitScope_Global, 0, &unit_latency, &size);
  int64_t latency = static_cast<int64_t>(unit_latency * kMicrosPerSecond);
#if TARGET_OS_OSX
  // The default device, not the unit's current one: on a change
  // notification the unit may not have switched yet.
  AudioDeviceID device = GetDefaultOutputDevice();
  // Device latencies are in the device's frames, at its own rate.
  Float64 device_rate = GetNominalSampleRate(device);
  if (device != kAudioObjectUnknown && device_rate > 0) {
    UInt32 frames = GetDeviceProperty(device, kAudioDevicePropertyLatency) +
                    GetDeviceProperty(device, kAudioDevicePropertySafetyOffset) +
                    GetStreamLatency(device);
    latency += static_cast<int64_t>(frames * kMicrosPerSecond / device_rate);
  }
#elif TARGET_OS_IOS
  latency += AudioSessionOutputLatency();
#endif
  return latency;
}

void CoreAudioSink::Start() {
  if (unit_ && !started_) {
    started_ = AudioOutputUnitStart(unit_) == noErr;
  }
}

void CoreAudioSink::Stop() {
  if (unit_ && started_) {
    AudioOutputUnitStop(unit_);
    started_ = false;
  }
}

// static
OSStatus CoreAudioSink::OnRender(void* user_data,
                                 AudioUnitRenderActionFlags* flags,
                                 const AudioTimeStamp* timestamp,
                                 UInt32 bus,
                                 UInt32 frame_count,
                                 AudioBufferList* buffers) {
  auto* self = static_cast<CoreAudioSink*>(user_data);
  auto* destination = static_cast<float*>(buffers->mBuffers[0].mData);
  int64_t playout_time =
      (timestamp->mFlags & kAudioTimeStampHostTimeValid)
          ? HostTimeToMicros(timestamp->mHostTime)
          : NowMicros();
  playout_time += self->output_latency_;
  int written = self->callback_(destination, static_cast<int>(frame_count),
                                playout_time);
  if (written < static_cast<int>(frame_count)) {
    memset(destination + written * kChannels, 0,
           (frame_count - written) * sizeof(float) * kChannels);
  }
  return noErr;
}

}  // namespace lychee
