#define LOG_TAG "mpegts"

#include "backends/mp2t/ts_parser.h"

#include "base/log.h"

namespace lychee {

namespace {

// A PES packet larger than this is not a frame: an 8K intra frame is a
// few MB. Dropped rather than buffered without bound.
constexpr size_t kMaxPesSize = 32 << 20;
// PSI sections are at most 1024 bytes (PAT, PMT).
constexpr size_t kMaxSectionBuffer = 4096;

// CRC-32 of PSI sections (ISO/IEC 13818-1 Annex A): polynomial
// 0x04C11DB7, MSB first, no final XOR. Over a section including its
// CRC_32 field it is 0.
uint32_t Crc32(const uint8_t* data, size_t size) {
  static const auto kTable = [] {
    struct Table {
      uint32_t entries[256];
    } table{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i << 24;
      for (int bit = 0; bit < 8; ++bit) {
        crc = crc & 0x80000000 ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
      }
      table.entries[i] = crc;
    }
    return table;
  }();
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < size; ++i) {
    crc = (crc << 8) ^ kTable.entries[((crc >> 24) ^ data[i]) & 0xff];
  }
  return crc;
}

int64_t ReadTimestamp(const uint8_t* data) {
  return (static_cast<int64_t>((data[0] >> 1) & 7) << 30) |
         (static_cast<int64_t>(data[1]) << 22) |
         (static_cast<int64_t>(data[2] >> 1) << 15) |
         (static_cast<int64_t>(data[3]) << 7) | (data[4] >> 1);
}

// Stream ids whose PES packets have no optional header (Table 2-21).
bool HasPesHeader(uint8_t stream_id) {
  switch (stream_id) {
    case 0xbc:
    case 0xbe:
    case 0xbf:
    case 0xf0:
    case 0xf1:
    case 0xf2:
    case 0xf8:
    case 0xff:
      return false;
    default:
      return true;
  }
}

}  // namespace

TsParser::TsParser(Client* client) : client_(client) {}
TsParser::~TsParser() = default;

void TsParser::Parse(const uint8_t* packet, int64_t offset) {
  if (packet[0] != 0x47) {
    return;
  }
  const bool error = packet[1] & 0x80;
  const bool unit_start = packet[1] & 0x40;
  const int pid = ((packet[1] & 0x1f) << 8) | packet[2];
  const int scrambling = packet[3] >> 6;
  const int adaptation = (packet[3] >> 4) & 3;
  const int continuity = packet[3] & 0xf;

  Section* section = nullptr;
  Pes* pes = nullptr;
  if (pid == 0) {
    section = &pat_;
  } else if (pid == pmt_pid_) {
    section = &pmt_;
  } else {
    auto it = pes_.find(pid);
    if (it == pes_.end()) {
      return;
    }
    pes = &it->second;
  }
  int& last_continuity = section ? section->continuity : pes->continuity;
  if (error) {
    // transport_error_indicator: this packet is damaged; what it belongs
    // to cannot be trusted either.
    last_continuity = -1;
    if (section) {
      section->active = false;
    } else {
      pes->active = false;
    }
    return;
  }

  size_t position = 4;
  bool discontinuity = false;
  bool random_access = false;
  if (adaptation & 2) {
    const size_t length = packet[4];
    if (length > kTsPacketSize - 5) {
      return;
    }
    if (length > 0) {
      discontinuity = packet[5] & 0x80;
      random_access = packet[5] & 0x40;
    }
    position = 5 + length;
  }
  if (!(adaptation & 1) || position >= kTsPacketSize) {
    return;  // No payload: the counter does not advance.
  }

  bool lost = false;
  if (last_continuity >= 0 && !discontinuity) {
    if (continuity == last_continuity) {
      return;  // A duplicate packet (2.4.3.3).
    }
    lost = continuity != ((last_continuity + 1) & 0xf);
  }
  last_continuity = continuity;
  if (scrambling != 0) {
    return;
  }

  const uint8_t* payload = packet + position;
  const size_t size = kTsPacketSize - position;
  if (section) {
    if (lost) {
      section->active = false;
    }
    ParseSection(section, unit_start, payload, size);
  } else {
    ParsePes(pid, pes, unit_start, lost, random_access, payload, size, offset);
  }
}

void TsParser::ParseSection(Section* section,
                            bool unit_start,
                            const uint8_t* payload,
                            size_t size) {
  if (!unit_start) {
    if (section->active) {
      section->data.insert(section->data.end(), payload, payload + size);
      HandleSections(section);
    }
    return;
  }
  const size_t pointer = payload[0];
  if (pointer + 1 > size) {
    section->active = false;
    return;
  }
  // The bytes before the pointer end the previous section.
  if (section->active && pointer > 0) {
    section->data.insert(section->data.end(), payload + 1,
                         payload + 1 + pointer);
    HandleSections(section);
  }
  section->data.assign(payload + 1 + pointer, payload + size);
  section->active = true;
  HandleSections(section);
}

void TsParser::HandleSections(Section* section) {
  std::vector<uint8_t>& data = section->data;
  size_t used = 0;
  while (section->active && data.size() - used >= 3) {
    const uint8_t* start = data.data() + used;
    if (start[0] == 0xff) {
      // Stuffing: nothing more in this packet.
      section->active = false;
      break;
    }
    const size_t length = ((start[1] & 0xf) << 8) | start[2];
    if (length > 1021) {
      section->active = false;
      break;
    }
    if (data.size() - used < 3 + length) {
      break;
    }
    if (length >= 9 && (start[1] & 0x80) && Crc32(start, 3 + length) == 0) {
      // Table ids 0 (PAT) and 2 (PMT), with the section syntax. The
      // handlers may change pmt_pid_, never this buffer.
      if (start[0] == 0 && section == &pat_) {
        HandlePat(start, 3 + length);
      } else if (start[0] == 2 && section == &pmt_) {
        HandlePmt(start, 3 + length);
      }
    }
    used += 3 + length;
  }
  if (!section->active) {
    data.clear();
  } else {
    data.erase(data.begin(), data.begin() + static_cast<ptrdiff_t>(used));
    if (data.size() > kMaxSectionBuffer) {
      data.clear();
      section->active = false;
    }
  }
}

void TsParser::HandlePat(const uint8_t* data, size_t size) {
  if (!(data[5] & 1)) {
    return;  // Not yet applicable (current_next_indicator).
  }
  // Program loop between the 8-byte header and the CRC.
  for (size_t i = 8; i + 4 <= size - 4; i += 4) {
    const int program = (data[i] << 8) | data[i + 1];
    const int pid = ((data[i + 2] & 0x1f) << 8) | data[i + 3];
    if (program == 0) {
      continue;  // The network information table.
    }
    if (pid != pmt_pid_) {
      pmt_pid_ = pid;
      pmt_version_ = -1;
      pmt_ = Section();
    }
    return;
  }
}

void TsParser::HandlePmt(const uint8_t* data, size_t size) {
  if (!(data[5] & 1)) {
    return;
  }
  const int version = (data[5] >> 1) & 0x1f;
  if (version == pmt_version_) {
    return;  // Repeated a few times a second.
  }
  const size_t end = size - 4;
  size_t i = 12 + (((data[10] & 0xf) << 8) | data[11]);
  std::vector<TsStreamEntry> streams;
  while (i + 5 <= end) {
    TsStreamEntry entry;
    entry.stream_type = data[i];
    entry.pid = ((data[i + 1] & 0x1f) << 8) | data[i + 2];
    const size_t info_length = ((data[i + 3] & 0xf) << 8) | data[i + 4];
    i += 5;
    if (i + info_length > end) {
      return;  // Malformed: keep the previous version.
    }
    for (size_t d = i; d + 2 <= i + info_length;) {
      const uint8_t tag = data[d];
      const size_t length = data[d + 1];
      if (d + 2 + length > i + info_length) {
        break;
      }
      const uint8_t* body = data + d + 2;
      if (tag == 0x0a && length >= 3) {  // ISO_639_language_descriptor
        entry.language.assign(reinterpret_cast<const char*>(body), 3);
      } else if (tag == 0x6a) {  // AC-3 (ETSI EN 300 468)
        entry.ac3 = true;
      } else if (tag == 0x7a) {  // Enhanced AC-3
        entry.eac3 = true;
      }
      d += 2 + length;
    }
    i += info_length;
    if (entry.pid != 0 && entry.pid != 0x1fff && entry.pid != pmt_pid_) {
      streams.push_back(entry);
    }
  }
  pmt_version_ = version;

  // Streams that are gone end here; new ones start clean.
  std::map<int, Pes> kept;
  for (auto& [pid, pes] : pes_) {
    bool listed = false;
    for (const TsStreamEntry& entry : streams) {
      listed |= entry.pid == pid;
    }
    if (listed) {
      kept[pid] = std::move(pes);
    } else if (pes.active && pes.data.size() >= 6) {
      Emit(pid, &pes);
    }
  }
  pes_ = std::move(kept);
  for (const TsStreamEntry& entry : streams) {
    pes_[entry.pid];
  }
  client_->OnProgram(streams);
}

void TsParser::ParsePes(int pid,
                        Pes* pes,
                        bool unit_start,
                        bool lost,
                        bool random_access,
                        const uint8_t* payload,
                        size_t size,
                        int64_t offset) {
  if (lost) {
    // Damaged data still decodes to something close (FFmpeg passes it on
    // too); dropping every PES packet that lost a TS packet would lose
    // nearly every large frame on a lossy link.
    if (pes->continuity_errors++ % 100 == 0) {
      LOGW("PID %d: continuity errors: %d", pid, pes->continuity_errors);
    }
  }
  if (unit_start) {
    // A new PES packet: the previous one ended, even if it lost data on
    // the way.
    if (pes->active && pes->data.size() >= 6) {
      Emit(pid, pes);
    }
    pes->active = false;
    if (size < 6 || payload[0] != 0 || payload[1] != 0 || payload[2] != 1) {
      return;
    }
    pes->data.assign(payload, payload + size);
    pes->offset = offset;
    pes->random_access = random_access;
    pes->active = true;
  } else {
    if (!pes->active) {
      return;
    }
    if (pes->data.size() + size > kMaxPesSize) {
      LOGW("PID %d: PES packet over %zu bytes dropped", pid, kMaxPesSize);
      pes->active = false;
      return;
    }
    pes->data.insert(pes->data.end(), payload, payload + size);
  }
  // Known lengths (audio) end without waiting for the next packet.
  const size_t length = (pes->data[4] << 8) | pes->data[5];
  if (length != 0 && pes->data.size() >= 6 + length) {
    Emit(pid, pes);
  }
}

void TsParser::Emit(int pid, Pes* pes) {
  pes->active = false;
  const std::vector<uint8_t>& data = pes->data;
  const size_t length = (data[4] << 8) | data[5];
  size_t end = data.size();
  if (length != 0 && 6 + length < end) {
    end = 6 + length;
  }
  PesPacket packet;
  packet.pid = pid;
  packet.offset = pes->offset;
  packet.random_access = pes->random_access;
  size_t payload = 6;
  if (HasPesHeader(data[3])) {
    if (end < 9 || 9 + static_cast<size_t>(data[8]) > end) {
      return;
    }
    const uint8_t flags = data[7] >> 6;
    if (flags >= 2 && data[8] >= 5) {
      packet.pts = ReadTimestamp(&data[9]);
    }
    if (flags == 3 && data[8] >= 10) {
      packet.dts = ReadTimestamp(&data[14]);
    }
    payload = 9 + data[8];
  }
  packet.data = data.data() + payload;
  packet.size = end - payload;
  if (packet.size > 0) {
    client_->OnPes(packet);
  }
}

void TsParser::Flush() {
  for (auto& [pid, pes] : pes_) {
    if (pes.active && pes.data.size() >= 6) {
      Emit(pid, &pes);
    }
    pes.active = false;
  }
}

void TsParser::Reset() {
  for (auto& [pid, pes] : pes_) {
    pes.active = false;
    pes.continuity = -1;
  }
  pat_.active = false;
  pat_.continuity = -1;
  pmt_.active = false;
  pmt_.continuity = -1;
}

void TsParser::AdoptProgram(const TsParser& other) {
  Reset();
  pmt_pid_ = other.pmt_pid_;
  pmt_version_ = other.pmt_version_;
  pes_.clear();
  for (const auto& entry : other.pes_) {
    pes_[entry.first];
  }
}

}  // namespace lychee
