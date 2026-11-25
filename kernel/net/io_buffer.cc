#include "net/io_buffer.h"

#include <algorithm>

#include "lib/debug.h"
#include "machine/machine.h"
#include "memory/vmm.h"

namespace Net {

IoBuffer::IoBuffer(uint64_t size) : bytes(size) { ASSERT(size <= MAX); }

IoBuffer::~IoBuffer() {
  for (auto *f : frames) {
    if (f) physMem.free(PPN{PA{uint64_t(f) - Sys::hhdm_offset}});
  }
}

char *IoBuffer::frame(uint64_t i) {
  if (frames[i] == nullptr) frames[i] = (char *)VA{physMem.alloc()}.va();
  return frames[i];
}

void IoBuffer::write(uint64_t off, const void *src, uint64_t n) {
  ASSERT(off <= bytes && n <= bytes - off);
  auto *from = (const char *)src;
  while (n > 0) {
    uint64_t in = off % FRAME_SIZE;
    uint64_t chunk = std::min(n, FRAME_SIZE - in);
    memcpy(frame(off / FRAME_SIZE) + in, from, chunk);
    off += chunk;
    from += chunk;
    n -= chunk;
  }
}

void IoBuffer::read(uint64_t off, void *dst, uint64_t n) const {
  ASSERT(off <= bytes && n <= bytes - off);
  auto *to = (char *)dst;
  while (n > 0) {
    uint64_t in = off % FRAME_SIZE;
    uint64_t chunk = std::min(n, FRAME_SIZE - in);
    const char *f = frames[off / FRAME_SIZE];
    if (f) memcpy(to, f + in, chunk);
    else memset(to, 0, chunk);
    off += chunk;
    to += chunk;
    n -= chunk;
  }
}

uint64_t IoBuffer::copy_from_user(const char *user, uint64_t n) {
  ASSERT(n <= bytes);
  uint64_t done = 0;
  while (done < n) {
    uint64_t chunk = std::min(n - done, FRAME_SIZE);
    uint64_t left = VMM::copy_user_(frame(done / FRAME_SIZE), user + done, chunk);
    done += chunk - left;
    if (left) break;
  }
  return done;
}

uint64_t IoBuffer::copy_to_user(char *user, uint64_t n) const {
  ASSERT(n <= bytes);
  static const char zeros[FRAME_SIZE]{};
  uint64_t done = 0;
  while (done < n) {
    uint64_t chunk = std::min(n - done, FRAME_SIZE);
    const char *f = frames[done / FRAME_SIZE];
    uint64_t left = VMM::copy_user_(user + done, f ? f : zeros, chunk);
    done += chunk - left;
    if (left) break;
  }
  return done;
}

} // namespace Net
