#ifndef FLUTTER_PLUGIN_CLIENT_AUDIO_PLAYER_PLUGIN_H_
#define FLUTTER_PLUGIN_CLIENT_AUDIO_PLAYER_PLUGIN_H_


#include <windows.h>
#include <mfplay.h>
#include <flutter/method_channel.h>
#include <flutter/encodable_value.h>
#include <Shlwapi.h>

using namespace flutter;
using namespace std;

#if 0
static long long toInteger(LARGE_INTEGER const &integer) {
#ifdef INT64_MAX // Does the compiler natively support 64-bit integers?
    return integer.QuadPart;
#else
    return (static_cast<long long>(integer.HighPart) << 32) | integer.LowPart;
#endif
}
#endif

// signed long long to LARGE_INTEGER object:

static LARGE_INTEGER toLargeInteger(long long value) {
    LARGE_INTEGER result{};

#ifdef INT64_MAX // Does the compiler natively support 64-bit integers?
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

class MediaPlayerCallback : public IMFPMediaPlayerCallback {
    long m_cRef; // Reference count

public:

    MediaPlayerCallback() : m_cRef(1) {
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) {
        static const QITAB qit[] =
                {
                        QITABENT(MediaPlayerCallback, IMFPMediaPlayerCallback),
                        {0},
                };
        return QISearch(this, qit, riid, ppv);
    }

    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() {
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



class ClientAudioPlayer {
private:
    const WCHAR *url{};
    IMFPMediaPlayer *player{};
    MethodChannel<EncodableValue> *channel;
    MediaPlayerCallback *playerCallback{};

    void dispatchEvent(ClientPlayerEvent event, EncodableMap *params = nullptr);

public:
    string playerId;

    ClientAudioPlayer(const WCHAR *url, MethodChannel<EncodableValue> *channel, string playerId);

    ~ClientAudioPlayer();

    void play() {
        player->Play();
    }

    void pause() {
        player->Pause();
    }

    void seekTo(long long ms) {
        PROPVARIANT time{};
        time.hVal = toLargeInteger(ms);
        player->SetPosition(MFP_POSITIONTYPE_100NS, &time);
    }

};


#endif //FLUTTER_PLUGIN_CLIENT_AUDIO_PLAYER_PLUGIN_H_
