#pragma once

#include <algorithm>
#include <cstdint>

#include "memory/physmem.h"

namespace Net {

// A bounded FIFO of bytes in physical frames: a stream socket's receive
// buffer (Unix sockets, TCP's receive queue) or send buffer (TCP keeps sent
// bytes until they're acknowledged: peek at an offset, discard on ACK).
//
// Not synchronized: the owner holds its own lock around every call. Frames
// come and go with the data, an empty queue holds none.
class ByteQueue {
  struct Page {
    Page *next = nullptr;
    char *data;
    uint64_t start = 0; // first unread byte
    uint64_t end = 0;   // one past the last written byte
  };

  Page *head = nullptr;
  Page *tail = nullptr;
  uint64_t bytes = 0;

public:
  static constexpr uint64_t DEFAULT_CAPACITY = 64 * 1024;

  const uint64_t capacity;

  explicit ByteQueue(uint64_t capacity = DEFAULT_CAPACITY) : capacity(capacity) {}
  ~ByteQueue() { clear(); }
  ByteQueue(const ByteQueue &) = delete;
  ByteQueue &operator=(const ByteQueue &) = delete;

  uint64_t size() const { return bytes; }
  uint64_t space() const { return capacity - bytes; }
  bool empty() const { return bytes == 0; }

  // Appends up to n bytes (no more than space()): fill(dst, len) writes len
  // bytes at dst and is called once per contiguous piece, in order. Returns
  // how many were appended.
  template <typename F> uint64_t append(uint64_t n, F fill) {
    n = std::min(n, space());
    uint64_t done = 0;
    while (done < n) {
      if (tail == nullptr || tail->end == FRAME_SIZE) add_page();
      uint64_t chunk = std::min(n - done, FRAME_SIZE - tail->end);
      fill(tail->data + tail->end, chunk);
      tail->end += chunk;
      bytes += chunk;
      done += chunk;
    }
    return done;
  }

  // Visits up to n bytes starting offset bytes in, without removing them:
  // visit(src, len) once per contiguous piece, in order. Returns how many
  // were visited.
  template <typename F> uint64_t peek(uint64_t offset, uint64_t n, F visit) const {
    if (offset >= bytes) return 0;
    n = std::min(n, bytes - offset);
    uint64_t done = 0;
    for (Page *p = head; p && done < n; p = p->next) {
      uint64_t have = p->end - p->start;
      if (offset >= have) {
        offset -= have;
        continue;
      }
      uint64_t chunk = std::min(n - done, have - offset);
      visit(p->data + p->start + offset, chunk);
      done += chunk;
      offset = 0;
    }
    return done;
  }

  // Removes the first n bytes (no more than size())
  void discard(uint64_t n);

  void clear() { discard(bytes); }

private:
  void add_page();
};

} // namespace Net
