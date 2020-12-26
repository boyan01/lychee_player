#include "include/audio_player/client_audio_player.h"
#include <mfplay.h>
#include <utility>

ClientAudioPlayer::ClientAudioPlayer(const WCHAR *url, MethodChannel<EncodableValue> *channel, string playerId) {
    this->channel = channel;
    this->playerId = std::move(playerId);
    this->url = url;
    this->playerCallback = new MediaPlayerCallback();
    HRESULT hr = S_OK;
    hr = MFPCreateMediaPlayer(url, TRUE, 0, playerCallback, nullptr, &player);
    if (FAILED(hr)) {
        dispatchEvent(ClientPlayerEvent::Error);
    }
}

ClientAudioPlayer::~ClientAudioPlayer() {
    if (player) {
        player->ClearMediaItem();
        player->Release();
    }
}

void ClientAudioPlayer::dispatchEvent(ClientPlayerEvent event, EncodableMap *params) {
    if (!channel) {
        return;
    }
    auto p = make_unique<EncodableMap>();
    (*p)[EncodableValue("playerId")] = EncodableValue(playerId);
    (*p)[EncodableValue("event")] = EncodableValue(event);
    if (params) {
        for (auto &pair : *params) {
            (*p)[pair.first] = pair.second;
        }
    }
    auto value = EncodableValue(params);
    channel->InvokeMethod("onPlaybackEvent", unique_ptr<EncodableValue>(&value), nullptr);
}


void MediaPlayerCallback::OnMediaPlayerEvent(MFP_EVENT_HEADER *pEventHeader) {

}