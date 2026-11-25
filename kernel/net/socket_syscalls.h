#pragma once

#include <cstdint>

#include "async/sync.h"
#include "lib/shared.h"

namespace impl {
struct TCB;
class OpenFile;
} // namespace impl

namespace Net {

// The socket system calls (Syscall::SOCKET .. ACCEPT4), with their arguments
// in t's registers. syscall.cc's dispatch sends them all here.
sync<int64_t> socket_syscall(impl::TCB *t);

// read(2) and write(2) on a socket fd: recv and send with no flags
sync<int64_t> socket_read(StrongRef<impl::OpenFile> file, char *user_buf, uint64_t len);
sync<int64_t> socket_write(StrongRef<impl::OpenFile> file, const char *user_buf, uint64_t len);

} // namespace Net
