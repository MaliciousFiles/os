/*
 * AF_UNIX, SOCK_STREAM, from socketpair: two connected ends, each one's
 * writes are the other's reads. Java makes one of these before it opens any
 * network connection (its selector's wakeup channel).
 *
 * Named Unix sockets (bind/connect to a path) need the file system: not yet.
 */

#include "async/event.h"
#include "net/byte_queue.h"
#include "net/socket.h"
#include "syscall.h"

namespace Net {

namespace {

class UnixStreamSocket final : public SocketFile {
  // What both ends share. Queue i holds the bytes waiting for end i to read
  // them (end 1 - i wrote them).
  struct Pair {
    SpinLock lock;
    ByteQueue queue[2];
    Event has_data[2];  // queue i got bytes, or its reader should stop waiting
    Event has_space[2]; // queue i lost bytes, or its writer should stop waiting
    bool read_shut[2]{};  // end i reads no more (SHUT_RD, or closed)
    bool write_shut[2]{}; // end i writes no more (SHUT_WR, or closed)
    UnixStreamSocket *end[2]{}; // the ends still open, for poll notifications
  };

  StrongRef<Pair> pair;
  const int me;

  UnixStreamSocket(StrongRef<Pair> pair, int me) : SocketFile(AF_UNIX, SOCK_STREAM, 0), pair(pair), me(me) {}

  int peer() const { return 1 - me; }

  // What poll reports for end i. The pair's lock is held.
  static uint32_t mask_locked(const Pair &p, int i) {
    int other = 1 - i;
    bool eof = p.write_shut[other] || p.read_shut[i];
    bool write_fails = p.write_shut[i] || p.read_shut[other];
    uint32_t mask = 0;
    if (!p.queue[i].empty() || eof) mask |= Poll::IN | Poll::RDNORM;
    if (p.write_shut[other]) mask |= Poll::RDHUP;
    if (write_fails || p.queue[other].space() > 0) mask |= Poll::OUT | Poll::WRNORM;
    if (eof && write_fails) mask |= Poll::HUP;
    return mask;
  }

  // Tells both ends' pollers their readiness may have changed. The pair's
  // lock is held.
  static void notify_locked(Pair &p) {
    for (int i = 0; i < 2; i++) {
      if (p.end[i]) p.end[i]->poll_queue.notify(mask_locked(p, i));
    }
  }

public:
  static int64_t create_pair(int type, int protocol, StrongRef<impl::OpenFile> &a, StrongRef<impl::OpenFile> &b) {
    if (type != SOCK_STREAM) return Syscall::EPROTONOSUPPORT;
    if (protocol != 0) return Syscall::EPROTONOSUPPORT;
    StrongRef<Pair> pair{new Pair};
    auto *end0 = new UnixStreamSocket{pair, 0};
    auto *end1 = new UnixStreamSocket{pair, 1};
    pair->end[0] = end0;
    pair->end[1] = end1;
    a = StrongRef<impl::OpenFile>{end0};
    b = StrongRef<impl::OpenFile>{end1};
    return 0;
  }

  ~UnixStreamSocket() override {
    Pair &p = *pair.operator->();
    LockGuard g{p.lock};
    p.end[me] = nullptr;
    p.read_shut[me] = p.write_shut[me] = true;
    p.queue[me].clear(); // nobody will read it
    p.has_data[peer()].trigger(); // the peer's reads see the end of the stream
    p.has_space[me].trigger();    // the peer's writes fail (EPIPE)
    notify_locked(p);
  }

  uint32_t poll_mask() override {
    Pair &p = *pair.operator->();
    LockGuard g{p.lock};
    return mask_locked(p, me);
  }

  sync<int64_t> sendto(const IoBuffer &data, uint64_t len, int flags, const SockAddr *dest) override {
    if (dest) co_return Syscall::EISCONN; // like Linux: a socketpair end is always connected
    Pair &p = *pair.operator->();
    ByteQueue &q = p.queue[peer()];
    uint64_t sent = 0;
    LockGuard g{p.lock};
    while (sent < len) {
      if (p.write_shut[me] || p.read_shut[peer()]) co_return sent ? int64_t(sent) : Syscall::EPIPE;
      if (q.space() == 0) {
        if (flags & MSG_DONTWAIT) co_return sent ? int64_t(sent) : Syscall::EAGAIN;
        co_await p.has_space[peer()].wait(p.lock);
        if (impl::current_thread_dead()) co_return sent ? int64_t(sent) : Syscall::EINTR;
        continue;
      }
      sent += q.append(len - sent, [&data, at = sent](char *dst, uint64_t n) mutable {
        data.read(at, dst, n);
        at += n;
      });
      p.has_data[peer()].trigger();
      notify_locked(p);
    }
    co_return int64_t(sent);
  }

  sync<int64_t> recvfrom(IoBuffer &data, uint64_t len, int flags, SockAddr *src) override {
    Pair &p = *pair.operator->();
    ByteQueue &q = p.queue[me];
    LockGuard g{p.lock};
    while (q.empty()) {
      if (len == 0 || p.write_shut[peer()] || p.read_shut[me]) co_return 0;
      if (flags & MSG_DONTWAIT) co_return Syscall::EAGAIN;
      co_await p.has_data[me].wait(p.lock);
      if (impl::current_thread_dead()) co_return Syscall::EINTR;
    }
    uint64_t n = q.peek(0, len, [&data, at = uint64_t(0)](const char *bytes, uint64_t count) mutable {
      data.write(at, bytes, count);
      at += count;
    });
    if (!(flags & MSG_PEEK)) {
      q.discard(n);
      p.has_space[me].trigger();
      notify_locked(p);
    }
    if (src) *src = unix_unnamed();
    co_return int64_t(n);
  }

  int64_t shutdown(int how) override {
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) return Syscall::EINVAL;
    Pair &p = *pair.operator->();
    LockGuard g{p.lock};
    if (how != SHUT_WR) {
      p.read_shut[me] = true;
      p.has_data[me].trigger();  // our readers see the end
      p.has_space[me].trigger(); // the peer's writers fail
    }
    if (how != SHUT_RD) {
      p.write_shut[me] = true;
      p.has_data[peer()].trigger();  // the peer's readers see the end
      p.has_space[peer()].trigger(); // our writers fail
    }
    notify_locked(p);
    return 0;
  }

  // like Linux: a socketpair end is connected, not listening (and has no name to listen on)
  int64_t listen(int) override { return Syscall::EINVAL; }
  sync<int64_t> accept(StrongRef<impl::OpenFile> &, SockAddr *, bool) override { co_return Syscall::EINVAL; }

  int64_t getsockname(SockAddr &out) override {
    out = unix_unnamed();
    return 0;
  }

  int64_t getpeername(SockAddr &out) override {
    out = unix_unnamed();
    return 0;
  }
};

[[maybe_unused]] const RegisterSocketType unix_stream{AF_UNIX, SOCK_STREAM, nullptr, UnixStreamSocket::create_pair};

} // namespace

} // namespace Net
