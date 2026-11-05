#ifndef LYCHEE_MEDIA_DECODE_ORDER_TIMESTAMPS_H_
#define LYCHEE_MEDIA_DECODE_ORDER_TIMESTAMPS_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

#include "media/annexb.h"
#include "media/encoded_packet.h"
#include "media/stream_info.h"

namespace lychee {

// Some MP4s with B-frames have no composition offsets (ctts): every
// sample's pts is its dts, in decode order, and a decoder then shows the
// pictures it reorders at the wrong times, so the picture jumps back and
// forth. This puts H.264 packets back on display times: it works out each
// picture's order (POC, ITU-T H.264 8.2.1) from its slice header and
// hands the stream's times out in that order, as a decoder's reorder
// buffer would output the pictures. It sees each packet as the decoder
// will, a few packets ahead, and gives out the packets in decode order.
//
// It is active while the stream's packets have pts == dts; a packet with
// another pts ends it (the container's times are display times). On
// streams without B-frames it changes nothing.
class DecodeOrderTimestamps {
 public:
  // Inactive unless |config| is H.264 that frames are not due right
  // after their packets (live input).
  void Initialize(const VideoConfig& config);

  // Takes a packet in decode order (not end of stream).
  void Push(std::shared_ptr<EncodedPacket> packet);
  // Releases every packet held, e.g. before the end of the stream.
  void Drain();
  // The next packet ready for the decoder, or null.
  std::shared_ptr<EncodedPacket> Pop();
  // Forgets the packets held and the picture order, for a seek.
  void Reset();

 private:
  struct Sps {
    int log2_max_frame_num = 4;
    int poc_type = 0;
    int log2_max_poc_lsb = 4;
    bool frame_mbs_only = true;
    bool separate_planes = false;
    // Pictures that can precede one in decode order and follow it in
    // output order.
    size_t reorder = 0;
  };
  struct Pps {
    int sps = 0;
    bool bottom_field_poc = false;
  };
  struct Held {
    std::shared_ptr<EncodedPacket> packet;
    int64_t order = 0;
    bool timed = false;
  };
  struct Slot {
    int64_t time;
    int64_t duration;
    bool operator>(const Slot& other) const { return time > other.time; }
  };

  void ParseSps(const uint8_t* nal, size_t size);
  void ParsePps(const uint8_t* nal, size_t size);
  // The picture order of the first slice in |packet|, false if there is
  // none it can tell. |*idr| is true for an IDR picture.
  bool PictureOrder(const EncodedPacket& packet, int64_t* order, bool* idr);
  bool SliceOrder(const uint8_t* nal, size_t size, int64_t* order, bool* idr);
  // Gives the earliest time left to the held picture first in output
  // order without a time.
  void TimeNext();
  void Release();
  void Stop();

  bool enabled_ = false;
  // Whether the stream's times still look like decode times.
  bool active_ = false;
  AnnexBConverter nal_;
  std::map<uint32_t, Sps> sps_;
  std::map<uint32_t, Pps> pps_;
  std::deque<Held> held_;
  // Times of the held packets, earliest first.
  std::vector<Slot> slots_;
  std::deque<std::shared_ptr<EncodedPacket>> ready_;
  size_t untimed_ = 0;
  // Of the last slice's SPS.
  size_t reorder_ = 16;
  // Picture order where it is decode order (POC type 2).
  int64_t decoded_ = 0;
  // Of the last reference picture (8.2.1.1).
  bool first_ = true;
  int64_t previous_msb_ = 0;
  int64_t previous_lsb_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_DECODE_ORDER_TIMESTAMPS_H_
