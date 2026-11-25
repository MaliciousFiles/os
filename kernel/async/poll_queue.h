#pragma once

#include <cstdint>

#include "async/spin_lock.h"

/*
 * Readiness notifications: the base that poll, ppoll and epoll stand on.
 *
 * Every OpenFile has a PollQueue. A file reports its readiness two ways:
 *   - OpenFile::poll_mask() says what is ready right now (a Poll:: mask)
 *   - whenever that may have changed, the file calls poll_queue.notify(mask)
 *     with the new mask
 *
 * A waiter (one poll() call, one epoll registration) adds a PollWaiter to the
 * file's queue, THEN reads poll_mask(): in that order a change between the
 * two can't be missed.
 *
 * Lock order: a file's own lock -> its PollQueue's lock -> the waiter's own
 * lock. So PollWaiter::wake and file_closed run with the file's lock possibly
 * held: they must not block, and must not call back into any file.
 */
namespace Poll {
// Linux's values (poll.h); epoll's EPOLL* events use the same bits
constexpr uint32_t IN = 0x001;
constexpr uint32_t PRI = 0x002;
constexpr uint32_t OUT = 0x004;
constexpr uint32_t ERR = 0x008;
constexpr uint32_t HUP = 0x010;
constexpr uint32_t NVAL = 0x020;
constexpr uint32_t RDNORM = 0x040;
constexpr uint32_t RDBAND = 0x080;
constexpr uint32_t WRNORM = 0x100;
constexpr uint32_t WRBAND = 0x200;
constexpr uint32_t RDHUP = 0x2000;
} // namespace Poll

class PollQueue;

class PollWaiter {
  friend class PollQueue;
  PollWaiter *prev = nullptr;
  PollWaiter *next = nullptr;
  bool linked = false;

public:
  virtual ~PollWaiter() = default;

  // The file's readiness may have changed, mask is what it is now.
  virtual void wake(uint32_t mask) = 0;

  // The file is going away (its last reference was dropped). The waiter is
  // already unlinked: it won't be called again.
  virtual void file_closed() {}
};

class PollQueue {
  SpinLock lock;
  PollWaiter *head = nullptr;

public:
  PollQueue() = default;
  PollQueue(const PollQueue &) = delete;
  PollQueue &operator=(const PollQueue &) = delete;

  ~PollQueue() {
    LockGuard g{lock};
    while (head) {
      PollWaiter *w = head;
      unlink(w);
      w->file_closed();
    }
  }

  void add(PollWaiter *w) {
    LockGuard g{lock};
    if (w->linked) return;
    w->prev = nullptr;
    w->next = head;
    if (head) head->prev = w;
    head = w;
    w->linked = true;
  }

  // false: it wasn't in the queue (never added, already removed, or the file
  // went away and file_closed was called)
  bool remove(PollWaiter *w) {
    LockGuard g{lock};
    if (!w->linked) return false;
    unlink(w);
    return true;
  }

  void notify(uint32_t mask) {
    LockGuard g{lock};
    for (PollWaiter *w = head; w; w = w->next) w->wake(mask);
  }

private:
  void unlink(PollWaiter *w) {
    if (w->prev) w->prev->next = w->next;
    else head = w->next;
    if (w->next) w->next->prev = w->prev;
    w->prev = w->next = nullptr;
    w->linked = false;
  }
};
