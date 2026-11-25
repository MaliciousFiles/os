#pragma once

#include <cstdint>

#include "lib/debug.h"
#include "memory/physmem.h"
#include "memory/vmm.h"

namespace Net {

class NetIf;

// One packet in one physical frame, with room in front so each layer going
// down can prepend its header without copying (UDP/TCP, then IPv4, then
// Ethernet), and each layer going up strips its own.
//
//   [ headroom | data ........ | tailroom ]
//              ^head           ^tail
//
// Ownership: whoever holds the UniqueRef<Packet> owns it. Handing a packet
// to the next layer (ip_output, NetIf::transmit, a protocol's input) gives
// it away.
struct Packet {
  static constexpr uint32_t CAPACITY = FRAME_SIZE;
  // Ethernet (14) + IPv4 with options (60) + TCP with options (60), rounded
  static constexpr uint32_t HEADROOM = 160;
  static constexpr uint32_t MAX_PAYLOAD = CAPACITY - HEADROOM;

  NetIf *netif = nullptr; // receive side: the interface it arrived on

  Packet() : frame(physMem.alloc()), buf((uint8_t *)VA{frame}.va()) {}
  ~Packet() { physMem.free(frame); }
  Packet(const Packet &) = delete;
  Packet &operator=(const Packet &) = delete;

  uint8_t *data() { return buf + head; }
  uint32_t length() const { return tail - head; }

  // Makes room for n bytes in front (a header going down), returns them
  uint8_t *prepend(uint32_t n) {
    ASSERT(n <= head);
    head -= n;
    return buf + head;
  }
  // Adds n bytes at the end (the payload), returns them
  uint8_t *append(uint32_t n) {
    ASSERT(n <= CAPACITY - tail);
    uint8_t *p = buf + tail;
    tail += n;
    return p;
  }
  // Removes n bytes from the front (a header going up)
  void strip(uint32_t n) {
    ASSERT(n <= length());
    head += n;
  }
  // Keeps only the first n bytes (drops Ethernet padding, say)
  void truncate(uint32_t n) {
    if (n < length()) tail = head + n;
  }
  // Starts over empty, with the full headroom (to fill a received frame)
  void reset(uint32_t headroom = HEADROOM) { head = tail = headroom; }

private:
  PPN frame;
  uint8_t *buf;
  uint32_t head = HEADROOM;
  uint32_t tail = HEADROOM;
};

} // namespace Net
