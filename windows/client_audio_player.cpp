#include "include/audio_player/client_audio_player.h"

#include <mfplay.h>

#include <utility>
#include <flutter/texture_registrar.h>


int64_t system_clock_systemTime() {
    int64_t tick = static_cast<int64_t>(GetTickCount());
    return tick;
}

ClientAudioPlayer::ClientAudioPlayer(const WCHAR *url, shared_ptr<MethodChannel<EncodableValue>> channel,
                                     const char *playerId) : m_cRef(1) {
    this->channel = channel;
    this->playerId = new string(playerId);
    this->url = url;
    HRESULT hr = S_OK;
    hr = MFPCreateMediaPlayer(url, FALSE, 0, this, nullptr, &player);
    if (FAILED(hr)) {
        dispatchEvent(ClientPlayerEvent::Error);
    } else {
        dispatchEvent(ClientPlayerEvent::Prepared);
    }
}

ClientAudioPlayer::~ClientAudioPlayer() {
    if (player) {
        player->ClearMediaItem();
        player->Shutdown();
    }
}

void ClientAudioPlayer::dispatchEvent(ClientPlayerEvent event, EncodableMap *params) {
    if (!channel) {
        return;
    }
    auto map = EncodableMap();
    map[EncodableValue("playerId")] = EncodableValue(playerId->c_str());
    map[EncodableValue("event")] = EncodableValue(event);
    if (params) {
        for (auto &pair : *params) {
            map[pair.first] = pair.second;
        }
    }
    channel->InvokeMethod("onPlaybackEvent", make_unique<EncodableValue>(map));
}

void ClientAudioPlayer::OnMediaPlayerEvent(MFP_EVENT_HEADER *pEventHeader) {
    switch (pEventHeader->eEventType) {
        case MFP_EVENT_TYPE_PLAY:
            dispatchEvent(ClientPlayerEvent::Playing);
            break;
        case MFP_EVENT_TYPE_PAUSE:
            dispatchEvent(ClientPlayerEvent::Paused);
            break;
        case MFP_EVENT_TYPE_MEDIAITEM_SET: {
            auto event = MFP_GET_MEDIAITEM_SET_EVENT(pEventHeader);
            PROPVARIANT time{};
            event->pMediaItem->GetDuration(MFP_POSITIONTYPE_100NS, &time);
            dispatchEvent(ClientPlayerEvent::Prepared,
                          &EncodableMap({{EncodableValue("duration"), EncodableValue(toInteger(time.hVal) / 10000)}}));
        }
            break;
        case MFP_EVENT_TYPE_ERROR:
            dispatchEvent(ClientPlayerEvent::Error);
            return;
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            dispatchEvent(ClientPlayerEvent::End);
            break;
        default:
            break;
    }
    MFP_MEDIAPLAYER_STATE state{};
    player->GetState(&state);
    auto playing = state == MFP_MEDIAPLAYER_STATE_PLAYING;
    PROPVARIANT position{};
    player->GetPosition(MFP_POSITIONTYPE_100NS, &position);
    dispatchEvent(ClientPlayerEvent::OnIsPlayingChanged,
                  &EncodableMap({{EncodableValue("playing"),    EncodableValue(playing)},
                                 {EncodableValue("position"),   EncodableValue(toInteger(position.hVal) / 10000)},
                                 {EncodableValue("updateTime"), EncodableValue(system_clock_systemTime())}}));
}
