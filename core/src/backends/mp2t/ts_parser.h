#ifndef LYCHEE_BACKENDS_MP2T_TS_PARSER_H_
#define LYCHEE_BACKENDS_MP2T_TS_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lychee {

inline constexpr size_t kTsPacketSize = 188;

// An elementary stream of the program, as its PMT lists it.
struct TsStreamEntry {
  int pid = -1;
  int stream_type = 0;
  // ISO 639 language descriptor, or empty.
  std::string language;
  // stream_type 0x06 (private data) with an AC-3 or E-AC-3 descriptor.
  bool ac3 = false;
  bool eac3 = false;
  // stream_type 0x06 with a descriptor of other media: DVB subtitles,
  // teletext, DTS, AAC or an extension (AC-4, ...).
  bool other_media = false;
};

// One PES packet's payload: an access unit, or several (audio).
struct PesPacket {
  int pid = -1;
  const uint8_t* data = nullptr;
  size_t size = 0;
  // 33-bit 90 kHz timestamps as the stream carries them, or -1.
  int64_t pts = -1;
  int64_t dts = -1;
  // Where the TS packet that started it begins in the input.
  int64_t offset = 0;
  // The adaptation field's random_access_indicator on that packet.
  bool random_access = false;
};

// Splits 188-byte transport stream packets (ISO/IEC 13818-1) into the
// first program's PMT and its streams' PES packets. Pure parsing: the
// caller reads the input and finds packet boundaries.
//
// PES packets that lost TS packets (continuity counter errors) are
// handed out as they are, as FFmpeg does: decoders conceal the damage,
// and ADTS framing resyncs.
class TsParser {
 public:
  class Client {
   public:
    virtual ~Client() = default;
    // A new program, or a new version of it (streams added or replaced).
    virtual void OnProgram(const std::vector<TsStreamEntry>& streams) = 0;
    // Valid during the call only.
    virtual void OnPes(const PesPacket& pes) = 0;
  };

  explicit TsParser(Client* client);
  ~TsParser();

  // |packet| is kTsPacketSize bytes starting with the sync byte; |offset|
  // is where it (or its M2TS timecode) starts in the input.
  void Parse(const uint8_t* packet, int64_t offset);

  // The input ended: hands out PES packets of unknown length.
  void Flush();

  // Drops packets in progress and continuity state, e.g. before reading
  // from elsewhere in the input. The program stays known.
  void Reset();

  // Takes |other|'s program, as if its PAT and PMT had been read: for
  // reading from the middle of an input whose program is known.
  void AdoptProgram(const TsParser& other);

  bool has_program() const { return pmt_version_ >= 0; }

 private:
  struct Section {
    std::vector<uint8_t> data;
    int continuity = -1;
    // A hash of the last packet's payload, for telling duplicates.
    uint64_t payload_hash = 0;
    bool active = false;
  };
  struct Pes {
    std::vector<uint8_t> data;
    int continuity = -1;
    uint64_t payload_hash = 0;
    bool active = false;
    int64_t offset = 0;
    bool random_access = false;
    int continuity_errors = 0;
  };

  void ParseSection(Section* section,
                    bool unit_start,
                    const uint8_t* payload,
                    size_t size);
  void HandleSections(Section* section);
  void HandlePat(const uint8_t* data, size_t size);
  void HandlePmt(const uint8_t* data, size_t size);
  void ParsePes(int pid,
                Pes* pes,
                bool unit_start,
                bool lost,
                bool random_access,
                const uint8_t* payload,
                size_t size,
                int64_t offset);
  // Hands |pes| out if it is a well-formed PES packet; ends it either way.
  void Emit(int pid, Pes* pes);

  Client* const client_;
  Section pat_;
  Section pmt_;
  int pmt_pid_ = -1;
  int pmt_version_ = -1;
  std::map<int, Pes> pes_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MP2T_TS_PARSER_H_
