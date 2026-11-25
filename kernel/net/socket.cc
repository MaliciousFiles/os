#include "net/socket.h"

#include "lib/debug.h"
#include "machine/machine.h"
#include "net/byte_order.h"
#include "syscall.h"

namespace Net {

namespace {

struct SocketType {
  int domain;
  int type;
  CreateSocket create;
  CreateSocketPair create_pair;
};

// constant-initialized: register_socket_type can run from any static
// initializer (they run one at a time, before the other cores start)
constexpr uint32_t MAX_TYPES = 16;
SocketType types[MAX_TYPES];
uint32_t type_count = 0;

// 0 and the registration, or an error for an unknown domain/type
int64_t find_type(int domain, int type, const SocketType *&out) {
  bool known_domain = false;
  for (uint32_t i = 0; i < type_count; i++) {
    if (types[i].domain != domain) continue;
    known_domain = true;
    if (types[i].type == type) {
      out = &types[i];
      return 0;
    }
  }
  return known_domain ? Syscall::EPROTONOSUPPORT : Syscall::EAFNOSUPPORT;
}

// copies an int option out (Linux writes at most len bytes of it)
int64_t get_int(int v, uint8_t *value, uint32_t &len) {
  len = len < sizeof(int) ? len : uint32_t(sizeof(int));
  memcpy(value, &v, len);
  return 0;
}

int64_t set_int(int &v, const uint8_t *value, uint32_t len) {
  if (len < sizeof(int)) return Syscall::EINVAL;
  memcpy(&v, value, sizeof(int));
  return 0;
}

template <typename T> int64_t get_struct(const T &v, uint8_t *value, uint32_t &len) {
  len = len < sizeof(T) ? len : uint32_t(sizeof(T));
  memcpy(value, &v, len);
  return 0;
}

template <typename T> int64_t set_struct(T &v, const uint8_t *value, uint32_t len) {
  if (len < sizeof(T)) return Syscall::EINVAL;
  memcpy(&v, value, sizeof(T));
  return 0;
}

} // namespace

int64_t to_inet(const SockAddr &sa, Ipv4Addr &addr, uint16_t &port) {
  if (sa.len < sizeof(SockAddrIn)) return Syscall::EINVAL;
  if (sa.family() != AF_INET) return Syscall::EAFNOSUPPORT;
  SockAddrIn in;
  memcpy(&in, sa.bytes, sizeof(in));
  addr = ntoh32(in.addr);
  port = ntoh16(in.port);
  return 0;
}

SockAddr from_inet(Ipv4Addr addr, uint16_t port) {
  SockAddrIn in{.family = AF_INET, .port = hton16(port), .addr = hton32(addr), .zero = {}};
  SockAddr sa;
  sa.len = sizeof(in);
  memcpy(sa.bytes, &in, sizeof(in));
  return sa;
}

SockAddr unix_unnamed() {
  SockAddr sa;
  sa.len = sizeof(uint16_t);
  sa.bytes[0] = AF_UNIX;
  return sa;
}

int64_t SocketFile::bind(const SockAddr &) { return Syscall::EOPNOTSUPP; }
int64_t SocketFile::listen(int) { return Syscall::EOPNOTSUPP; }
sync<int64_t> SocketFile::accept(StrongRef<impl::OpenFile> &, SockAddr *, bool) { co_return Syscall::EOPNOTSUPP; }
sync<int64_t> SocketFile::connect(const SockAddr &, bool) { co_return Syscall::EOPNOTSUPP; }
int64_t SocketFile::shutdown(int) { return Syscall::ENOTCONN; }
int64_t SocketFile::getsockname(SockAddr &) { return Syscall::EOPNOTSUPP; }
int64_t SocketFile::getpeername(SockAddr &) { return Syscall::ENOTCONN; }

int64_t SocketFile::setsockopt(int level, int name, const uint8_t *value, uint32_t len) {
  if (level != SOL_SOCKET) return Syscall::ENOPROTOOPT;
  LockGuard g{options_lock};
  switch (name) {
  case SO_REUSEADDR: return set_int(options.reuse_addr, value, len);
  case SO_REUSEPORT: return set_int(options.reuse_port, value, len);
  case SO_KEEPALIVE: return set_int(options.keepalive, value, len);
  case SO_BROADCAST: return set_int(options.broadcast, value, len);
  case SO_SNDBUF: return set_int(options.sndbuf, value, len);
  case SO_RCVBUF: return set_int(options.rcvbuf, value, len);
  case SO_LINGER: return set_struct(options.linger, value, len);
  case SO_RCVTIMEO: return set_struct(options.rcvtimeo, value, len);
  case SO_SNDTIMEO: return set_struct(options.sndtimeo, value, len);
  default: return Syscall::ENOPROTOOPT;
  }
}

int64_t SocketFile::getsockopt(int level, int name, uint8_t *value, uint32_t &len) {
  if (level != SOL_SOCKET) return Syscall::ENOPROTOOPT;
  switch (name) {
  case SO_TYPE: return get_int(type, value, len);
  case SO_DOMAIN: return get_int(domain, value, len);
  case SO_PROTOCOL: return get_int(protocol, value, len);
  case SO_ERROR: return get_int(int(-take_error()), value, len);
  default: break;
  }
  LockGuard g{options_lock};
  switch (name) {
  case SO_REUSEADDR: return get_int(options.reuse_addr, value, len);
  case SO_REUSEPORT: return get_int(options.reuse_port, value, len);
  case SO_KEEPALIVE: return get_int(options.keepalive, value, len);
  case SO_BROADCAST: return get_int(options.broadcast, value, len);
  case SO_SNDBUF: return get_int(options.sndbuf, value, len);
  case SO_RCVBUF: return get_int(options.rcvbuf, value, len);
  case SO_LINGER: return get_struct(options.linger, value, len);
  case SO_RCVTIMEO: return get_struct(options.rcvtimeo, value, len);
  case SO_SNDTIMEO: return get_struct(options.sndtimeo, value, len);
  default: return Syscall::ENOPROTOOPT;
  }
}

// Kernel callers only: the syscalls use sendto/recvfrom directly (see
// socket_syscalls.cc), these keep a socket a usable OpenFile.
sync<uint64_t> SocketFile::read(char *buf, uint64_t len) {
  if (len > IoBuffer::MAX) len = IoBuffer::MAX;
  IoBuffer data{len};
  int64_t n = co_await recvfrom(data, len, is_nonblocking() ? MSG_DONTWAIT : 0, nullptr);
  if (n > 0) data.read(0, buf, uint64_t(n) < len ? uint64_t(n) : len);
  co_return uint64_t(n);
}

sync<uint64_t> SocketFile::write(char *buf, uint64_t len) {
  if (len > IoBuffer::MAX) len = IoBuffer::MAX;
  IoBuffer data{len};
  data.write(0, buf, len);
  co_return uint64_t(co_await sendto(data, len, is_nonblocking() ? MSG_DONTWAIT : 0, nullptr));
}

void register_socket_type(int domain, int type, CreateSocket create, CreateSocketPair create_pair) {
  ASSERT(type_count < MAX_TYPES);
  types[type_count++] = {domain, type, create, create_pair};
}

int64_t create_socket(int domain, int type, int protocol, StrongRef<impl::OpenFile> &out) {
  const SocketType *t = nullptr;
  if (auto error = find_type(domain, type, t)) return error;
  if (t->create == nullptr) return Syscall::EOPNOTSUPP;
  return t->create(type, protocol, out);
}

int64_t create_socket_pair(int domain, int type, int protocol, StrongRef<impl::OpenFile> &a,
                           StrongRef<impl::OpenFile> &b) {
  const SocketType *t = nullptr;
  if (auto error = find_type(domain, type, t)) return error;
  if (t->create_pair == nullptr) return Syscall::EOPNOTSUPP;
  return t->create_pair(type, protocol, a, b);
}

} // namespace Net
