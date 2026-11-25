#pragma once

#include <cstdint>

#include "async/sync.h"
#include "net/io_buffer.h"
#include "net/ipv4.h"
#include "structures/pcb.h"

/*
 * Sockets.
 *
 * A socket is an OpenFile (FileType SOCKET) in the process's fd table, like a
 * pipe. Every kind of socket (Unix stream, UDP, TCP) is a subclass of
 * SocketFile that implements the operations it supports; the rest fail the
 * way Linux fails them.
 *
 * The syscalls (net/socket_syscalls.cc) do everything that isn't specific to
 * a kind of socket: look up the fd, copy addresses and data in and out of
 * user memory, apply O_NONBLOCK, install new fds. So the methods here only
 * see kernel memory (SockAddr, IoBuffer), never a user pointer: they may hold
 * a spin lock while copying.
 *
 * Blocking: a method that has to wait co_awaits an Event under its own lock
 * (see UnixStreamSocket), unless it was asked not to block: send/recv get
 * MSG_DONTWAIT in flags, accept/connect get nonblock. Those return EAGAIN
 * (connect: EINPROGRESS) instead. After waking up, check
 * impl::current_thread_dead() and give up if it's set.
 *
 * Readiness: override poll_mask() and call poll_queue.notify(mask) whenever
 * it may have changed (async/poll_queue.h).
 *
 * New kinds of sockets register themselves (RegisterSocketType) from their
 * own .cc file: nothing central needs editing to add one.
 */
namespace Net {

// Linux's values (sys/socket.h, netinet/in.h)
constexpr int AF_UNSPEC = 0;
constexpr int AF_UNIX = 1;
constexpr int AF_INET = 2;
constexpr int AF_INET6 = 10;

constexpr int SOCK_STREAM = 1;
constexpr int SOCK_DGRAM = 2;
constexpr int SOCK_TYPE_MASK = 0xF;
constexpr int SOCK_NONBLOCK = 04000;    // = O_NONBLOCK
constexpr int SOCK_CLOEXEC = 02000000;  // = O_CLOEXEC

constexpr int IPPROTO_IP = 0;
constexpr int IPPROTO_TCP = 6;
constexpr int IPPROTO_UDP = 17;

// send/recv flags
constexpr int MSG_PEEK = 0x2;
constexpr int MSG_TRUNC = 0x20;
constexpr int MSG_DONTWAIT = 0x40;
constexpr int MSG_WAITALL = 0x100;
constexpr int MSG_NOSIGNAL = 0x4000;

// shutdown's how
constexpr int SHUT_RD = 0;
constexpr int SHUT_WR = 1;
constexpr int SHUT_RDWR = 2;

// socket options: levels, then names
constexpr int SOL_SOCKET = 1;
constexpr int SO_REUSEADDR = 2;
constexpr int SO_TYPE = 3;
constexpr int SO_ERROR = 4;
constexpr int SO_BROADCAST = 6;
constexpr int SO_SNDBUF = 7;
constexpr int SO_RCVBUF = 8;
constexpr int SO_KEEPALIVE = 9;
constexpr int SO_LINGER = 13;
constexpr int SO_REUSEPORT = 15;
constexpr int SO_RCVTIMEO = 20;
constexpr int SO_SNDTIMEO = 21;
constexpr int SO_PROTOCOL = 38;
constexpr int SO_DOMAIN = 39;

constexpr int TCP_NODELAY = 1; // level IPPROTO_TCP

constexpr int IP_TOS = 1; // level IPPROTO_IP
constexpr int IP_TTL = 2;
constexpr int IP_MULTICAST_IF = 32;
constexpr int IP_MULTICAST_TTL = 33;
constexpr int IP_MULTICAST_LOOP = 34;
constexpr int IP_ADD_MEMBERSHIP = 35;
constexpr int IP_DROP_MEMBERSHIP = 36;

// A socket address in kernel memory: the bytes of a struct sockaddr_*
// exactly as the user passed them (len of them), or as a socket fills one in.
struct SockAddr {
  static constexpr uint32_t MAX = 128; // sizeof(struct sockaddr_storage)
  uint32_t len = 0;
  alignas(8) uint8_t bytes[MAX]{};

  uint16_t family() const { return len >= 2 ? uint16_t(bytes[0] | (bytes[1] << 8)) : uint16_t(AF_UNSPEC); }
};

// struct sockaddr_in: port and addr in network byte order
struct [[gnu::packed]] SockAddrIn {
  uint16_t family;
  uint16_t port;
  uint32_t addr;
  uint8_t zero[8];
};
static_assert(sizeof(SockAddrIn) == 16);

// sockaddr_in <-> address and port (host byte order). to_inet returns 0, or
// EAFNOSUPPORT / EINVAL for something that isn't an IPv4 address.
int64_t to_inet(const SockAddr &sa, Ipv4Addr &addr, uint16_t &port);
SockAddr from_inet(Ipv4Addr addr, uint16_t port);
// An unnamed Unix socket's address: just the family
SockAddr unix_unnamed();

class SocketFile : public impl::OpenFile {
protected:
  SocketFile(int domain, int type, int protocol)
      : OpenFile(impl::SOCKET, true, true), domain(domain), type(type), protocol(protocol) {}

public:
  const int domain;   // AF_*
  const int type;     // SOCK_STREAM or SOCK_DGRAM (no flags)
  const int protocol; // IPPROTO_*, 0 for Unix sockets

  // nullptr if file isn't a socket
  static SocketFile *from(impl::OpenFile *file) {
    return file && file->type == impl::SOCKET ? static_cast<SocketFile *>(file) : nullptr;
  }

  virtual int64_t bind(const SockAddr &addr);
  virtual int64_t listen(int backlog);
  // On success, out is the new connection's socket and peer (if not null) its
  // address
  virtual sync<int64_t> accept(StrongRef<impl::OpenFile> &out, SockAddr *peer, bool nonblock);
  virtual sync<int64_t> connect(const SockAddr &addr, bool nonblock);

  // Sends the first len bytes of data (to dest if not null, else to the
  // connected peer). Returns how many were sent, or an error. A stream socket
  // may send fewer than len only if it would block (MSG_DONTWAIT) or after an
  // error, a datagram socket sends all of them or none.
  virtual sync<int64_t> sendto(const IoBuffer &data, uint64_t len, int flags, const SockAddr *dest) = 0;

  // Receives up to len bytes into data (and the sender's address into src if
  // not null). Returns how many, 0 at the end of a stream, or an error. A
  // datagram longer than len is cut short; with MSG_TRUNC it returns the
  // full length anyway.
  virtual sync<int64_t> recvfrom(IoBuffer &data, uint64_t len, int flags, SockAddr *src) = 0;

  virtual int64_t shutdown(int how);
  virtual int64_t getsockname(SockAddr &out);
  virtual int64_t getpeername(SockAddr &out);

  // SOL_SOCKET options are kept here: a subclass handles its own levels (and
  // the SOL_SOCKET options that change its behavior) and passes everything
  // else on to these. value is in kernel memory; getsockopt sets len to how
  // many bytes it wrote.
  virtual int64_t setsockopt(int level, int name, const uint8_t *value, uint32_t len);
  virtual int64_t getsockopt(int level, int name, uint8_t *value, uint32_t &len);

  // read(2) and write(2) on a socket are recv and send with no flags
  sync<uint64_t> read(char *buf, uint64_t len) override;
  sync<uint64_t> write(char *buf, uint64_t len) override;

  // SO_ERROR: an error that happened in the background (a refused
  // connection, say). Reading it clears it.
  void set_error(int64_t error) { __atomic_store_n(&pending_error, error, __ATOMIC_RELEASE); }
  int64_t take_error() { return __atomic_exchange_n(&pending_error, 0, __ATOMIC_ACQ_REL); }

protected:
  // The SOL_SOCKET options this layer only stores (and reports back). A
  // subclass reads the ones it honors (rcvbuf, broadcast, ...).
  struct Options {
    int reuse_addr = 0;
    int reuse_port = 0;
    int keepalive = 0;
    int broadcast = 0;
    int sndbuf = 64 * 1024;
    int rcvbuf = 64 * 1024;
    struct Linger { int on, seconds; } linger{0, 0};
    struct Timeval { int64_t sec, usec; } rcvtimeo{0, 0}, sndtimeo{0, 0};
  };
  SpinLock options_lock;
  Options options;

private:
  int64_t pending_error = 0;
};

// Makes a socket of a registered type. type has no SOCK_* flags.
using CreateSocket = int64_t (*)(int type, int protocol, StrongRef<impl::OpenFile> &out);
using CreateSocketPair = int64_t (*)(int type, int protocol, StrongRef<impl::OpenFile> &a, StrongRef<impl::OpenFile> &b);

// Registers what socket(domain, type, ...) and socketpair(domain, type, ...)
// make. Either may be nullptr (not supported: EOPNOTSUPP). Use
// RegisterSocketType from a static initializer.
void register_socket_type(int domain, int type, CreateSocket create, CreateSocketPair create_pair);

struct RegisterSocketType {
  RegisterSocketType(int domain, int type, CreateSocket create, CreateSocketPair create_pair) {
    register_socket_type(domain, type, create, create_pair);
  }
};

int64_t create_socket(int domain, int type, int protocol, StrongRef<impl::OpenFile> &out);
int64_t create_socket_pair(int domain, int type, int protocol, StrongRef<impl::OpenFile> &a, StrongRef<impl::OpenFile> &b);

} // namespace Net
