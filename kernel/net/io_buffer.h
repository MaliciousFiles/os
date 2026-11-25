#pragma once

#include <cstdint>

#include "memory/physmem.h"

namespace Net {

// A kernel copy of the bytes of one send or receive, up to MAX bytes, in
// physical frames (the kernel heap is small, and a blocked receiver shouldn't
// pin heap memory).
//
// Why copy at all? A file's lock is a spin lock, and touching user memory can
// fault and block: so the syscall copies the user's bytes into an IoBuffer
// first (send), or out of one after the socket filled it (receive), never
// while a socket lock is held.
//
// Frames are allocated as bytes are written into them: a receiver that is
// blocked waiting holds none.
class IoBuffer {
public:
  static constexpr uint64_t FRAMES = 16;
  static constexpr uint64_t MAX = FRAMES * FRAME_SIZE; // 64 KiB: a whole UDP datagram

  explicit IoBuffer(uint64_t size);
  ~IoBuffer();
  IoBuffer(const IoBuffer &) = delete;
  IoBuffer &operator=(const IoBuffer &) = delete;

  uint64_t size() const { return bytes; }

  // [off, off + n) must be within size()
  void write(uint64_t off, const void *src, uint64_t n);
  void read(uint64_t off, void *dst, uint64_t n) const; // never-written bytes read as 0

  // Copy n bytes from/to user memory at offset 0. Return how many were
  // copied: fewer than n means a fault (EFAULT) at that point.
  uint64_t copy_from_user(const char *user, uint64_t n);
  uint64_t copy_to_user(char *user, uint64_t n) const;

private:
  char *frame(uint64_t i);
  char *frames[FRAMES]{};
  uint64_t bytes;
};

} // namespace Net
