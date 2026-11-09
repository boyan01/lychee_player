#define LOG_TAG "merge"

#include "media/merging_demuxer.h"

#include <algorithm>
#include <thread>
#include <utility>

#include "base/log.h"

namespace lychee {

MergingDemuxer::MergingDemuxer(std::unique_ptr<Demuxer> main,
                               std::vector<std::unique_ptr<Demuxer>> audio,
                               std::vector<std::string> audio_urls)
    : audio_urls_(std::move(audio_urls)) {
  inputs_.resize(1 + audio.size());
  inputs_[0].demuxer = std::move(main);
  for (size_t i = 0; i < audio.size(); ++i) {
    inputs_[1 + i].demuxer = std::move(audio[i]);
  }
}

MergingDemuxer::~MergingDemuxer() = default;

const char* MergingDemuxer::name() const {
  return inputs_[0].demuxer->name();
}

bool MergingDemuxer::Open(const std::string& url, std::string* error) {
  // The audio inputs open while the main one does: each costs a round
  // trip or more over the network.
  std::vector<std::string> errors(inputs_.size());
  std::vector<char> opened(inputs_.size(), 0);
  std::vector<std::thread> threads;
  threads.reserve(audio_urls_.size());
  for (size_t i = 1; i < inputs_.size(); ++i) {
    threads.emplace_back([this, i, &errors, &opened] {
      opened[i] = inputs_[i].demuxer->Open(audio_urls_[i - 1], &errors[i]);
    });
  }
  opened[0] = inputs_[0].demuxer->Open(url, &errors[0]);
  if (!opened[0]) {
    // No use waiting for the others.
    for (size_t i = 1; i < inputs_.size(); ++i) {
      inputs_[i].demuxer->Abort();
    }
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  if (!opened[0]) {
    *error = errors[0];
    return false;
  }

  const MediaInfo& main = inputs_[0].demuxer->media_info();
  media_info_ = main;
  inputs_[0].streams.resize(main.streams.size());
  for (size_t s = 0; s < main.streams.size(); ++s) {
    inputs_[0].streams[s] = static_cast<int>(s);
  }
  for (size_t i = 1; i < inputs_.size(); ++i) {
    const std::string& audio_url = audio_urls_[i - 1];
    if (!opened[i]) {
      *error = audio_url + ": " + errors[i];
      return false;
    }
    const MediaInfo& info = inputs_[i].demuxer->media_info();
    media_info_.seekable = media_info_.seekable && info.seekable;
    Input& input = inputs_[i];
    input.streams.assign(info.streams.size(), -1);
    for (size_t s = 0; s < info.streams.size(); ++s) {
      if (info.streams[s].type != MediaType::kAudio) {
        continue;
      }
      StreamInfo stream = info.streams[s];
      stream.index = static_cast<int>(media_info_.streams.size());
      input.streams[s] = stream.index;
      // Each file marks its own as the default: only the first is.
      stream.default_track = media_info_.default_audio_stream < 0;
      if (stream.default_track) {
        media_info_.default_audio_stream = stream.index;
      }
      media_info_.streams.push_back(std::move(stream));
    }
    if (std::find_if(input.streams.begin(), input.streams.end(),
                     [](int s) { return s >= 0; }) == input.streams.end()) {
      *error = audio_url + ": no audio";
      return false;
    }
  }
  LOGI("%zu audio inputs beside the main one, %zu streams", inputs_.size() - 1,
       media_info_.streams.size());
  return true;
}

void MergingDemuxer::SetEnabledStreams(const std::vector<int>& stream_indices) {
  for (Input& input : inputs_) {
    std::vector<int> local;
    for (size_t s = 0; s < input.streams.size(); ++s) {
      if (input.streams[s] >= 0 &&
          std::find(stream_indices.begin(), stream_indices.end(),
                    input.streams[s]) != stream_indices.end()) {
        local.push_back(static_cast<int>(s));
      }
    }
    input.demuxer->SetEnabledStreams(local);
    input.enabled = !local.empty();
    // What was read ahead of a stream now disabled goes.
    if (input.has_next &&
        std::find(stream_indices.begin(), stream_indices.end(),
                  input.next.stream_index) == stream_indices.end()) {
      input.has_next = false;
      input.next = {};
    }
  }
}

DemuxerReadResult MergingDemuxer::Read(EncodedPacket* packet) {
  for (Input& input : inputs_) {
    while (input.enabled && !input.ended && !input.has_next) {
      const DemuxerReadResult result = input.demuxer->Read(&input.next);
      if (result == DemuxerReadResult::kEndOfStream) {
        input.ended = true;
      } else if (result == DemuxerReadResult::kError) {
        error_ = input.demuxer->error();
        if (&input != &inputs_[0]) {
          error_ = audio_urls_[&input - &inputs_[1]] + ": " + error_;
        }
        return result;
      } else if (result == DemuxerReadResult::kAborted) {
        return result;
      } else if (input.next.stream_index >= 0 &&
                 static_cast<size_t>(input.next.stream_index) <
                     input.streams.size() &&
                 input.streams[input.next.stream_index] >= 0) {
        input.next.stream_index = input.streams[input.next.stream_index];
        input.has_next = true;
      }
    }
  }
  // The earliest in decode order (a packet without times first).
  Input* earliest = nullptr;
  int64_t earliest_time = 0;
  for (Input& input : inputs_) {
    if (!input.has_next) {
      continue;
    }
    const int64_t time =
        input.next.dts != kNoTimestamp ? input.next.dts : input.next.pts;
    if (!earliest || time < earliest_time) {
      earliest = &input;
      earliest_time = time;
    }
  }
  if (!earliest) {
    return DemuxerReadResult::kEndOfStream;
  }
  *packet = std::move(earliest->next);
  earliest->next = {};
  earliest->has_next = false;
  return DemuxerReadResult::kOk;
}

bool MergingDemuxer::Seek(int64_t time) {
  // Each one, also after a failure: a seek clears an interrupt.
  bool ok = true;
  for (Input& input : inputs_) {
    ok = input.demuxer->Seek(time) && ok;
    input.ended = false;
    input.has_next = false;
    input.next = {};
  }
  return ok;
}

void MergingDemuxer::Interrupt() {
  for (Input& input : inputs_) {
    input.demuxer->Interrupt();
  }
}

void MergingDemuxer::Abort() {
  for (Input& input : inputs_) {
    input.demuxer->Abort();
  }
}

int64_t MergingDemuxer::download_rate() const {
  return inputs_[0].demuxer->download_rate();
}

std::vector<VariantInfo> MergingDemuxer::variants() const {
  return inputs_[0].demuxer->variants();
}

int MergingDemuxer::current_variant() const {
  return inputs_[0].demuxer->current_variant();
}

void MergingDemuxer::SetVariantPolicy(int pinned, int64_t max_bandwidth) {
  inputs_[0].demuxer->SetVariantPolicy(pinned, max_bandwidth);
}

bool MergingDemuxer::live_window(LiveWindow* window) const {
  return inputs_[0].demuxer->live_window(window);
}

}  // namespace lychee
