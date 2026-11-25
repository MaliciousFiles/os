#include "net/netif.h"

#include "async/async.h"

namespace Net {

namespace {

NetIf *registered = nullptr; // constant-initialized: safe from any static initializer

// Delivers on the event loop, after transmit returned
async<void> deliver_later(UniqueRef<Packet> packet, NetIf *netif) {
  packet->netif = netif;
  ip_input(packet, *netif);
  co_return;
}

class Loopback final : public NetIf {
public:
  Loopback() : NetIf("lo", IPV4_LOOPBACK, ipv4(255, 0, 0, 0), 1500) { register_netif(this); }

  bool is_loopback() const override { return true; }

  void transmit(UniqueRef<Packet> packet, Ipv4Addr) override { deliver_later(packet, this); }
};

Loopback lo;

} // namespace

void register_netif(NetIf *netif) {
  NetIf *head = __atomic_load_n(&registered, __ATOMIC_ACQUIRE);
  do {
    netif->next_registered = head;
  } while (!__atomic_compare_exchange_n(&registered, &head, netif, false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
}

NetIf *netifs() { return __atomic_load_n(&registered, __ATOMIC_ACQUIRE); }

NetIf &loopback() { return lo; }

} // namespace Net
