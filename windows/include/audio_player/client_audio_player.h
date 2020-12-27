#ifndef FLUTTER_PLUGIN_CLIENT_AUDIO_PLAYER_PLUGIN_H_
#define FLUTTER_PLUGIN_CLIENT_AUDIO_PLAYER_PLUGIN_H_

#include <Shlwapi.h>
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <mfplay.h>
#include <windows.h>

using namespace flutter;
using namespace std;

static long long toInteger(LARGE_INTEGER const &integer) {
#ifdef INT64_MAX  // Does the compiler natively support 64-bit integers?
  return integer.QuadPart;
#else
  return (static_cast<long long>(integer.HighPart) << 32) | integer.LowPart;
#endif
}

// signed long long to LARGE_INTEGER object:

static LARGE_INTEGER toLargeInteger(long long value) {
  LARGE_INTEGER result{};

#ifdef INT64_MAX  // Does the compiler natively support 64-bit integers?
  result.QuadPart = value;
#else
  result.high_part = value & 0xFFFFFFFF00000000;
  result.low_part = value & 0xFFFFFFFF;
#endif
  return result;
}

enum ClientPlayerEvent {
  Paused,
  Playing,
  Preparing,
  Prepared,
  BufferingStart,
  BufferingEnd,
  Error,
  Seeking,
  SeekFinished,
  End,
  UpdateBufferPosition,
  OnIsPlayingChanged
};

class ClientAudioPlayer : public IMFPMediaPlayerCallback {
 private:
  const WCHAR *url{};
  IMFPMediaPlayer *player;
  shared_ptr<MethodChannel<EncodableValue>> channel;

  void dispatchEvent(ClientPlayerEvent event, EncodableMap *params = nullptr);

  long m_cRef;  // Reference count

 public:
  string *playerId;

  ClientAudioPlayer(const WCHAR *url, shared_ptr<MethodChannel<EncodableValue>> channel, const char *playerId);

  ~ClientAudioPlayer();

  void play() {
    if (player) {
      player->Play();
    }
  }

  void pause() {
    if (player) {
      player->Pause();
    }
  }

  void seekTo(long long ms) {
    if (ms < 0 || !player) {
      std::cerr << "can not seek to" << ms << endl;
      return;
    }
    PROPVARIANT time{};
    time.vt = VT_I8;
    time.hVal = toLargeInteger(ms * 10000);
    HRESULT result = player->SetPosition(MFP_POSITIONTYPE_100NS, &time);
    if (FAILED(result)) {
    std:
      cerr << "seek to " << ms << " failed, result code : " << result << endl;
    }
  }

  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) {
    static const QITAB qit[] =
        {
            QITABENT(ClientAudioPlayer, IMFPMediaPlayerCallback),
            {0},
        };
    return QISearch(this, qit, riid, ppv);
  }

  STDMETHODIMP_(ULONG)
  AddRef() {
    return InterlockedIncrement(&m_cRef);
  }

  STDMETHODIMP_(ULONG)
  Release() {
    ULONG count = InterlockedDecrement(&m_cRef);
    if (count == 0) {
      delete this;
      return 0;
    }
    return count;
  }

  // IMFPMediaPlayerCallback methods
  void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER *pEventHeader);
};

#endif  //FLUTTER_PLUGIN_CLIENT_AUDIO_PLAYER_PLUGIN_H_
