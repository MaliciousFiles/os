#include "net/ipv4.h"

#include "net/byte_order.h"
#include "net/netif.h"
#include "syscall.h"

namespace Net {

namespace {

// constant-initialized: register_protocol can run from any static initializer
ProtocolHandler handlers[256];

uint16_t next_id = 0;

bool is_ours(Ipv4Addr dst, const NetIf &netif) {
  if (netif.is_loopback()) return true; // only we send on it
  if (dst == IPV4_BROADCAST || is_multicast(dst) || dst == netif.addr) return true;
  // the subnet's broadcast address
  return netif.netmask != 0xFFFFFFFF && dst == (netif.addr | ~netif.netmask);
}

} // namespace

void register_protocol(uint8_t protocol, ProtocolHandler handler) { handlers[protocol] = handler; }

Route route(Ipv4Addr dst) {
  NetIf *first_up = nullptr; // the first interface that isn't loopback
  NetIf *gateway = nullptr;  // the first one with a default route
  for (NetIf *n = netifs(); n; n = n->next_registered) {
    // to ourselves: loopback, whichever interface has the address
    if (!n->is_loopback() && dst == n->addr) return {&loopback(), dst, dst};
    if (n->is_loopback()) continue;
    if (first_up == nullptr) first_up = n;
    if (gateway == nullptr && n->gateway) gateway = n;
    if (n->on_link(dst)) return {n, dst, n->addr};
  }
  // 0.0.0.0 means this host, like Linux
  if (is_loopback(dst) || dst == IPV4_ANY) return {&loopback(), dst, IPV4_LOOPBACK};
  // multicast and broadcast go out on the first real interface, to themselves
  if (is_multicast(dst) || dst == IPV4_BROADCAST) {
    if (first_up) return {first_up, dst, first_up->addr};
    return {nullptr, 0, 0};
  }
  if (gateway) return {gateway, gateway->gateway, gateway->addr};
  return {nullptr, 0, 0};
}

int64_t ip_output(UniqueRef<Packet> packet, Ipv4Addr src, Ipv4Addr dst, uint8_t protocol, uint8_t ttl) {
  Route r = route(dst);
  if (r.netif == nullptr) return Syscall::ENETUNREACH;
  if (packet->length() + sizeof(Ipv4Header) > r.netif->mtu) return Syscall::EMSGSIZE;

  auto *h = (Ipv4Header *)packet->prepend(sizeof(Ipv4Header));
  h->version_ihl = 0x45;
  h->tos = 0;
  h->total_length = hton16(uint16_t(packet->length()));
  h->id = hton16(__atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED));
  h->flags_fragment = hton16(0x4000); // DF: we don't fragment
  h->ttl = ttl;
  h->protocol = protocol;
  h->checksum = 0;
  h->src = hton32(src ? src : r.src);
  h->dst = hton32(dst);
  h->checksum = checksum_finish(checksum_add(h, sizeof(Ipv4Header)));

  r.netif->transmit(packet, r.next_hop);
  return 0;
}

void ip_input(UniqueRef<Packet> packet, NetIf &netif) {
  if (packet->length() < sizeof(Ipv4Header)) return;
  auto *h = (Ipv4Header *)packet->data();
  if ((h->version_ihl >> 4) != 4) return;
  uint32_t header_length = uint32_t(h->version_ihl & 0xF) * 4;
  uint32_t total_length = ntoh16(h->total_length);
  if (header_length < sizeof(Ipv4Header) || total_length < header_length || total_length > packet->length()) return;
  if (checksum_finish(checksum_add(h, header_length)) != 0) return;
  if (ntoh16(h->flags_fragment) & 0x3FFF) return; // a fragment (MF or an offset): no reassembly yet

  Ipv4Info info{
      .src = ntoh32(h->src),
      .dst = ntoh32(h->dst),
      .protocol = h->protocol,
      .ttl = h->ttl,
      .netif = &netif,
  };
  if (!is_ours(info.dst, netif)) return;

  ProtocolHandler handler = handlers[info.protocol];
  if (handler == nullptr) return; // ICMP protocol unreachable, once there's ICMP

  packet->truncate(total_length); // Ethernet pads short frames
  packet->strip(header_length);
  packet->netif = &netif;
  handler(packet, info);
}

uint32_t checksum_add(const void *data, uint32_t len, uint32_t sum) {
  auto *p = (const uint8_t *)data;
  for (; len > 1; len -= 2, p += 2) sum += (uint32_t(p[0]) << 8) | p[1];
  if (len) sum += uint32_t(p[0]) << 8;
  return sum;
}

uint16_t checksum_finish(uint32_t sum) {
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return hton16(uint16_t(~sum));
}

uint32_t pseudo_header_sum(Ipv4Addr src, Ipv4Addr dst, uint8_t protocol, uint16_t length) {
  return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + protocol + length;
}

} // namespace Net
