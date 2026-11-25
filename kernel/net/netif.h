#pragma once

#include <cstdint>

#include "lib/shared.h"
#include "net/ipv4.h"
#include "net/packet.h"

/*
 * A network interface: a device IPv4 sends through (virtio-net, loopback).
 *
 * An interface owns its link layer. transmit gets an IPv4 packet and the
 * next hop's IPv4 address. Ethernet interfaces resolve the next hop's MAC
 * (ARP), prepend the Ethernet header and queue the frame. Loopback just
 * hands it back up.
 *
 * Receiving: the interface strips its link-layer header and calls ip_input,
 * on the event loop (from a polling task or an interrupt's bottom half),
 * never from inside transmit: TCP sends ACKs from its input path while
 * holding its locks.
 */
namespace Net {

class NetIf {
public:
  const char *const name;
  Ipv4Addr addr;
  Ipv4Addr netmask;
  Ipv4Addr gateway = 0; // 0: no default route through this interface
  uint32_t mtu;         // the most bytes of IPv4 packet (header included) one transmit takes

  NetIf(const char *name, Ipv4Addr addr, Ipv4Addr netmask, uint32_t mtu)
      : name(name), addr(addr), netmask(netmask), mtu(mtu) {}
  virtual ~NetIf() = default;
  NetIf(const NetIf &) = delete;
  NetIf &operator=(const NetIf &) = delete;

  virtual bool is_loopback() const { return false; }

  // Sends one IPv4 packet (data starts at the IPv4 header, length <= mtu) to
  // next_hop, which is on this interface's link. Must not block, must not
  // call ip_input before it returns. May drop the packet (a full queue, no
  // ARP answer): IP is best effort, TCP retransmits.
  virtual void transmit(UniqueRef<Packet> packet, Ipv4Addr next_hop) = 0;

  bool on_link(Ipv4Addr a) const { return (a & netmask) == (addr & netmask); }

  NetIf *next_registered = nullptr;
};

// Adds an interface (for good: interfaces are never removed). Loopback adds
// itself at startup, a driver adds its interface once its device is up.
void register_netif(NetIf *netif);

// The registered interfaces, a list through next_registered. The list only
// grows, so walking it without a lock is safe.
NetIf *netifs();

NetIf &loopback();

} // namespace Net
