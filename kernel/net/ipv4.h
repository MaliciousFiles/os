#pragma once

#include <cstdint>

#include "lib/shared.h"
#include "net/packet.h"

/*
 * IPv4: between the transports (UDP, TCP) above and the interfaces (NetIf)
 * below.
 *
 * Going down: a transport builds its header in a Packet and calls
 * ip_output. IPv4 picks the interface (route), prepends its header, and hands
 * the packet to NetIf::transmit.
 *
 * Going up: an interface strips its link-layer header and calls ip_input.
 * IPv4 checks the header, strips it, and calls the handler registered for the
 * packet's protocol (register_protocol).
 *
 * Addresses are uint32_t in HOST byte order everywhere in the kernel (127.0.0.1
 * is 0x7F000001). Only headers on the wire are big-endian.
 *
 * Not here yet: fragmentation and reassembly (packets bigger than the MTU are
 * EMSGSIZE, fragments are dropped), IP options, ICMP.
 */
namespace Net {

using Ipv4Addr = uint32_t;

constexpr Ipv4Addr ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | d;
}

constexpr Ipv4Addr IPV4_ANY = 0;
constexpr Ipv4Addr IPV4_BROADCAST = 0xFFFFFFFF;
constexpr Ipv4Addr IPV4_LOOPBACK = ipv4(127, 0, 0, 1);

constexpr bool is_loopback(Ipv4Addr a) { return (a >> 24) == 127; }
constexpr bool is_multicast(Ipv4Addr a) { return (a >> 28) == 0xE; } // 224.0.0.0/4

// Protocol numbers (the IPv4 header's protocol field)
constexpr uint8_t PROTO_ICMP = 1;
constexpr uint8_t PROTO_TCP = 6;
constexpr uint8_t PROTO_UDP = 17;

struct [[gnu::packed]] Ipv4Header {
  uint8_t version_ihl;     // version 4 in the high nibble, header length in 32-bit words
  uint8_t tos;
  uint16_t total_length;   // big-endian, header + payload
  uint16_t id;             // big-endian
  uint16_t flags_fragment; // big-endian: DF 0x4000, MF 0x2000, offset in 8-byte units
  uint8_t ttl;
  uint8_t protocol;
  uint16_t checksum;
  uint32_t src;            // big-endian
  uint32_t dst;            // big-endian
};
static_assert(sizeof(Ipv4Header) == 20);

// What a protocol handler learns about a received packet (whose data now
// starts at the transport header)
struct Ipv4Info {
  Ipv4Addr src;
  Ipv4Addr dst;
  uint8_t protocol;
  uint8_t ttl;
  NetIf *netif;
};

// Called on the event loop, never from inside ip_output: a handler may send.
using ProtocolHandler = void (*)(UniqueRef<Packet> packet, const Ipv4Info &info);

// Registers the handler for a protocol number. Call it from a static
// initializer (see RegisterProtocol) so no shared init function needs editing.
void register_protocol(uint8_t protocol, ProtocolHandler handler);

struct RegisterProtocol {
  RegisterProtocol(uint8_t protocol, ProtocolHandler handler) { register_protocol(protocol, handler); }
};

// Where a packet to dst goes: the interface, the next hop on its link (dst
// itself, or the gateway), and the source address to send from. netif is
// nullptr if there's no route.
struct Route {
  NetIf *netif;
  Ipv4Addr next_hop;
  Ipv4Addr src;
};
Route route(Ipv4Addr dst);

// Sends packet (data = the transport header and payload) to dst. src 0 means
// the outgoing interface's address. Returns 0, or ENETUNREACH / EMSGSIZE.
// Transports that checksum a pseudo-header should pick src with route()
// first.
int64_t ip_output(UniqueRef<Packet> packet, Ipv4Addr src, Ipv4Addr dst, uint8_t protocol, uint8_t ttl = 64);

// An interface hands up a received packet whose data starts at the IPv4
// header. Drops what's broken or not for us.
void ip_input(UniqueRef<Packet> packet, NetIf &netif);

// The Internet checksum (RFC 1071) over len bytes, continuing from sum (pass
// the result of a previous partial sum, or 0). checksum_finish folds it into
// the 16-bit value that goes in a header (in network byte order already).
uint32_t checksum_add(const void *data, uint32_t len, uint32_t sum = 0);
uint16_t checksum_finish(uint32_t sum);

// The sum of the TCP/UDP pseudo-header, to start their checksums with
uint32_t pseudo_header_sum(Ipv4Addr src, Ipv4Addr dst, uint8_t protocol, uint16_t length);

} // namespace Net
