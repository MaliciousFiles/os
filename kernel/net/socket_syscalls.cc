/*
 * The socket system calls: everything about a socket syscall that isn't
 * specific to a kind of socket. Look up the fd, copy addresses, data and
 * options between user memory and kernel copies (SockAddr, IoBuffer), apply
 * O_NONBLOCK and SOCK_* flags, install new fds. The socket itself
 * (SocketFile) only ever sees kernel memory.
 */

#include "net/socket_syscalls.h"

#include <algorithm>

#include "memory/vmm.h"
#include "net/socket.h"
#include "structures/pcb.h"
#include "syscall.h"

namespace Net {

namespace {

using impl::OpenFile;
using impl::TCB;

bool user_range(uintptr_t start, uint64_t len) {
  if (start + len < start) return false;
  return start >= VMM::VA_START.va() && start + len <= VMM::VA_END;
}

bool read_user(void *dst, uintptr_t src, uint64_t len) {
  return user_range(src, len) && !VMM::copy_user_(dst, (const void *)src, len);
}

bool write_user(uintptr_t dst, const void *src, uint64_t len) {
  return user_range(dst, len) && !VMM::copy_user_((void *)dst, src, len);
}

// fd -> its socket. Keep file: it holds the socket alive across suspensions.
int64_t get_socket(TCB *t, uint64_t fd, StrongRef<OpenFile> &file, SocketFile *&socket) {
  file = t->process->fd_table.get(uint32_t(fd));
  if (file == nullptr) return Syscall::EBADF;
  socket = SocketFile::from(file.operator->());
  return socket ? 0 : Syscall::ENOTSOCK;
}

// A struct sockaddr from the user
int64_t copy_addr_in(uintptr_t user, uint64_t len, SockAddr &out) {
  if (int32_t(len) < 0 || len > SockAddr::MAX) return Syscall::EINVAL;
  out.len = uint32_t(len);
  return read_user(out.bytes, user, len) ? 0 : Syscall::EFAULT;
}

// A struct sockaddr to the user, the accept/recvfrom/getsockname way: *lenp
// says how much room there is, gets the address's full length.
int64_t copy_addr_out(const SockAddr &addr, uintptr_t user, uintptr_t lenp) {
  int32_t room;
  if (!read_user(&room, lenp, sizeof(room))) return Syscall::EFAULT;
  if (room < 0) return Syscall::EINVAL;
  uint32_t n = std::min(uint32_t(room), addr.len);
  if (!write_user(user, addr.bytes, n)) return Syscall::EFAULT;
  uint32_t full = addr.len;
  return write_user(lenp, &full, sizeof(full)) ? 0 : Syscall::EFAULT;
}

int64_t install(TCB *t, StrongRef<OpenFile> file, bool cloexec) {
  return int64_t(t->process->fd_table.open(file, cloexec));
}

// Sends len bytes from the user's buf: a stream in IoBuffer-sized pieces
// until it's all sent or one comes up short, a datagram in one piece.
sync<int64_t> send(SocketFile *socket, const char *buf, uint64_t len, int flags, const SockAddr *dest) {
  if (len && !user_range(uintptr_t(buf), len)) co_return Syscall::EFAULT;
  if (socket->is_nonblocking()) flags |= MSG_DONTWAIT;
  if (socket->type != SOCK_STREAM && len > IoBuffer::MAX) co_return Syscall::EMSGSIZE;

  uint64_t total = 0;
  do {
    uint64_t n = std::min(len - total, IoBuffer::MAX);
    IoBuffer data{n};
    uint64_t copied = data.copy_from_user(buf + total, n);
    if (copied == 0 && n > 0) co_return total ? int64_t(total) : Syscall::EFAULT;
    int64_t sent = co_await socket->sendto(data, copied, flags, dest);
    if (sent < 0) co_return total ? int64_t(total) : sent;
    total += uint64_t(sent);
    if (uint64_t(sent) < n) break; // short (would block, or a fault part way)
  } while (total < len);
  co_return int64_t(total);
}

// Receives up to len bytes (at most IoBuffer::MAX in one call) into the
// user's buf.
//   TODO: if the copy out faults, the bytes are already gone from the socket.
sync<int64_t> receive(SocketFile *socket, char *buf, uint64_t len, int flags, SockAddr *src) {
  if (len && !user_range(uintptr_t(buf), len)) co_return Syscall::EFAULT;
  if (socket->is_nonblocking()) flags |= MSG_DONTWAIT;

  uint64_t n = std::min(len, IoBuffer::MAX);
  IoBuffer data{n};
  int64_t got = co_await socket->recvfrom(data, n, flags, src);
  if (got <= 0) co_return got;
  uint64_t copy = std::min(uint64_t(got), n); // got > n: a datagram cut short, MSG_TRUNC
  if (data.copy_to_user(buf, copy) < copy) co_return Syscall::EFAULT;
  co_return got;
}

sync<int64_t> do_socket(TCB *t, int domain, int type, int protocol) {
  int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  type &= ~flags;
  if (type & ~SOCK_TYPE_MASK) co_return Syscall::EINVAL;

  StrongRef<OpenFile> file;
  if (auto error = create_socket(domain, type, protocol, file)) co_return error;
  file->set_nonblocking(flags & SOCK_NONBLOCK);
  co_return install(t, file, flags & SOCK_CLOEXEC);
}

sync<int64_t> do_socketpair(TCB *t, int domain, int type, int protocol, uintptr_t sv) {
  int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
  type &= ~flags;
  if (type & ~SOCK_TYPE_MASK) co_return Syscall::EINVAL;
  if (!user_range(sv, 2 * sizeof(int32_t))) co_return Syscall::EFAULT;

  StrongRef<OpenFile> a, b;
  if (auto error = create_socket_pair(domain, type, protocol, a, b)) co_return error;
  a->set_nonblocking(flags & SOCK_NONBLOCK);
  b->set_nonblocking(flags & SOCK_NONBLOCK);

  uint32_t fds[2];
  if (int64_t error = t->process->fd_table.open_pair(a, b, flags & SOCK_CLOEXEC, fds)) co_return error;
  if (!write_user(sv, fds, sizeof(fds))) {
    t->process->fd_table.close(fds[0]);
    t->process->fd_table.close(fds[1]);
    co_return Syscall::EFAULT;
  }
  co_return 0;
}

sync<int64_t> do_accept(TCB *t, uint64_t fd, uintptr_t addr, uintptr_t lenp, int flags) {
  if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) co_return Syscall::EINVAL;
  StrongRef<OpenFile> file;
  SocketFile *socket;
  if (auto error = get_socket(t, fd, file, socket)) co_return error;

  StrongRef<OpenFile> connection;
  SockAddr peer;
  if (int64_t error = co_await socket->accept(connection, &peer, socket->is_nonblocking())) co_return error;
  connection->set_nonblocking(flags & SOCK_NONBLOCK);
  // like Linux: a bad address loses the connection
  if (addr) {
    if (auto error = copy_addr_out(peer, addr, lenp)) co_return error;
  }
  co_return install(t, connection, flags & SOCK_CLOEXEC);
}

sync<int64_t> dispatch(TCB *t) {
  UserContext &c = t->context;
  uint64_t fd = c.rdi;

  switch (c.rax) {
  case Syscall::SOCKET: co_return co_await do_socket(t, int(c.rdi), int(c.rsi), int(c.rdx));
  case Syscall::SOCKETPAIR: co_return co_await do_socketpair(t, int(c.rdi), int(c.rsi), int(c.rdx), c.r10);
  case Syscall::ACCEPT: co_return co_await do_accept(t, fd, c.rsi, c.rdx, 0);
  case Syscall::ACCEPT4: co_return co_await do_accept(t, fd, c.rsi, c.rdx, int(c.r10));
  default: break;
  }

  // the rest work on an existing socket
  StrongRef<OpenFile> file;
  SocketFile *socket;
  if (auto error = get_socket(t, fd, file, socket)) co_return error;

  switch (c.rax) {
  case Syscall::BIND: {
    SockAddr addr;
    if (auto error = copy_addr_in(c.rsi, c.rdx, addr)) co_return error;
    co_return socket->bind(addr);
  }
  case Syscall::LISTEN: co_return socket->listen(int(c.rsi));
  case Syscall::CONNECT: {
    SockAddr addr;
    if (auto error = copy_addr_in(c.rsi, c.rdx, addr)) co_return error;
    co_return co_await socket->connect(addr, socket->is_nonblocking());
  }
  case Syscall::SENDTO: {
    // sendto(fd, buf, len, flags, dest, destlen)
    SockAddr dest;
    if (c.r8) {
      if (auto error = copy_addr_in(c.r8, c.r9, dest)) co_return error;
    }
    co_return co_await send(socket, (const char *)c.rsi, c.rdx, int(c.r10), c.r8 ? &dest : nullptr);
  }
  case Syscall::RECVFROM: {
    // recvfrom(fd, buf, len, flags, src, srclen)
    SockAddr src;
    int64_t got = co_await receive(socket, (char *)c.rsi, c.rdx, int(c.r10), c.r8 ? &src : nullptr);
    if (got >= 0 && c.r8) {
      if (auto error = copy_addr_out(src, c.r8, c.r9)) co_return error;
    }
    co_return got;
  }
  case Syscall::SHUTDOWN: co_return socket->shutdown(int(c.rsi));
  case Syscall::GETSOCKNAME:
  case Syscall::GETPEERNAME: {
    SockAddr addr;
    int64_t error = c.rax == Syscall::GETSOCKNAME ? socket->getsockname(addr) : socket->getpeername(addr);
    if (error) co_return error;
    co_return copy_addr_out(addr, c.rsi, c.rdx);
  }
  case Syscall::SETSOCKOPT: {
    // setsockopt(fd, level, name, value, len)
    constexpr uint32_t MAX_OPTION = 256;
    if (int32_t(c.r8) < 0) co_return Syscall::EINVAL;
    uint32_t len = std::min(uint32_t(c.r8), MAX_OPTION);
    uint8_t value[MAX_OPTION];
    if (len && !read_user(value, c.r10, len)) co_return Syscall::EFAULT;
    co_return socket->setsockopt(int(c.rsi), int(c.rdx), value, len);
  }
  case Syscall::GETSOCKOPT: {
    // getsockopt(fd, level, name, value, len*)
    constexpr uint32_t MAX_OPTION = 256;
    int32_t room;
    if (!read_user(&room, c.r8, sizeof(room))) co_return Syscall::EFAULT;
    if (room < 0) co_return Syscall::EINVAL;
    uint32_t len = std::min(uint32_t(room), MAX_OPTION);
    uint8_t value[MAX_OPTION]{};
    if (auto error = socket->getsockopt(int(c.rsi), int(c.rdx), value, len)) co_return error;
    if ((len && !write_user(c.r10, value, len)) || !write_user(c.r8, &len, sizeof(len))) co_return Syscall::EFAULT;
    co_return 0;
  }
  default: co_return Syscall::ENOSYS;
  }
}

} // namespace

sync<int64_t> socket_syscall(TCB *t) { co_return co_await dispatch(t); }

sync<int64_t> socket_read(StrongRef<OpenFile> file, char *user_buf, uint64_t len) {
  co_return co_await receive(SocketFile::from(file.operator->()), user_buf, len, 0, nullptr);
}

sync<int64_t> socket_write(StrongRef<OpenFile> file, const char *user_buf, uint64_t len) {
  co_return co_await send(SocketFile::from(file.operator->()), user_buf, len, 0, nullptr);
}

} // namespace Net
