#include "net/byte_queue.h"

#include "memory/vmm.h"

namespace Net {

void ByteQueue::add_page() {
  auto *page = new Page{.data = (char *)VA{physMem.alloc()}.va()};
  if (tail) tail->next = page;
  else head = page;
  tail = page;
}

void ByteQueue::discard(uint64_t n) {
  n = std::min(n, bytes);
  bytes -= n;
  while (head) {
    uint64_t chunk = std::min(n, head->end - head->start);
    head->start += chunk;
    n -= chunk;
    // Keep a partly read page that writes can still go into (the tail with
    // room), unless the queue is empty: an empty queue holds no frames.
    bool used_up = head->start == head->end;
    bool still_writable = head == tail && head->end < FRAME_SIZE;
    if (!used_up || (still_writable && bytes > 0)) break;
    Page *done = head;
    head = head->next;
    if (head == nullptr) tail = nullptr;
    physMem.free(PPN{PA{uint64_t(done->data) - Sys::hhdm_offset}});
    delete done;
  }
}

} // namespace Net
