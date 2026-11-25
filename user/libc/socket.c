#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

int fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  long arg = va_arg(ap, long);
  va_end(ap);
  return (int)syscall(SYS_fcntl, fd, cmd, arg, 0, 0, 0);
}

int socket(int domain, int type, int protocol) {
  return (int)syscall(SYS_socket, domain, type, protocol, 0, 0, 0);
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
  return (int)syscall(SYS_socketpair, domain, type, protocol, (long)sv, 0, 0);
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
  return (int)syscall(SYS_bind, fd, (long)addr, len, 0, 0, 0);
}

int listen(int fd, int backlog) { return (int)syscall(SYS_listen, fd, backlog, 0, 0, 0, 0); }

int accept(int fd, struct sockaddr *addr, socklen_t *len) {
  return (int)syscall(SYS_accept, fd, (long)addr, (long)len, 0, 0, 0);
}

int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags) {
  return (int)syscall(SYS_accept4, fd, (long)addr, (long)len, flags, 0, 0);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
  return (int)syscall(SYS_connect, fd, (long)addr, len, 0, 0, 0);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest, socklen_t dest_len) {
  return syscall(SYS_sendto, fd, (long)buf, (long)len, flags, (long)dest, dest_len);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src, socklen_t *src_len) {
  return syscall(SYS_recvfrom, fd, (long)buf, (long)len, flags, (long)src, (long)src_len);
}

ssize_t send(int fd, const void *buf, size_t len, int flags) { return sendto(fd, buf, len, flags, NULL, 0); }

ssize_t recv(int fd, void *buf, size_t len, int flags) { return recvfrom(fd, buf, len, flags, NULL, NULL); }

int shutdown(int fd, int how) { return (int)syscall(SYS_shutdown, fd, how, 0, 0, 0, 0); }

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
  return (int)syscall(SYS_getsockname, fd, (long)addr, (long)len, 0, 0, 0);
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
  return (int)syscall(SYS_getpeername, fd, (long)addr, (long)len, 0, 0, 0);
}

int setsockopt(int fd, int level, int name, const void *value, socklen_t len) {
  return (int)syscall(SYS_setsockopt, fd, level, name, (long)value, len, 0);
}

int getsockopt(int fd, int level, int name, void *value, socklen_t *len) {
  return (int)syscall(SYS_getsockopt, fd, level, name, (long)value, (long)len, 0);
}
