/*
 * The socket syscall layer and AF_UNIX socketpair, checked against Linux's
 * behavior: this program prints the same lines on Linux (build it with any
 * Linux C compiler and run it to compare).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

static void check(int ok, const char *name) {
  char line[128];
  const char *prefix = ok ? "ok   " : "FAIL ";
  size_t p = strlen(prefix), n = strlen(name);
  if (n > sizeof(line) - p - 1) n = sizeof(line) - p - 1;
  memcpy(line, prefix, p);
  memcpy(line + p, name, n);
  line[p + n] = '\0';
  puts(line);
  if (!ok) failures++;
}

#define FAILS_WITH(call, error) ((call) == -1 && errno == (error))

int main(void) {
  int sv[2];
  char buf[4096];

  check(FAILS_WITH(socket(200, SOCK_STREAM, 0), EAFNOSUPPORT), "socket: unknown family");
  check(FAILS_WITH(socket(AF_UNIX, SOCK_STREAM | 0x100, 0), EINVAL), "socket: unknown type flag");
  check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

  // data both ways
  check(write(sv[0], "hello", 5) == 5, "write 0->1");
  check(read(sv[1], buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0, "read 0->1");
  check(send(sv[1], "world", 5, 0) == 5, "send 1->0");
  check(recv(sv[0], buf, sizeof(buf), MSG_PEEK) == 5 && memcmp(buf, "world", 5) == 0, "recv MSG_PEEK");
  check(recv(sv[0], buf, sizeof(buf), 0) == 5 && memcmp(buf, "world", 5) == 0, "recv after peek");
  check(FAILS_WITH(recv(sv[0], buf, sizeof(buf), MSG_DONTWAIT), EAGAIN), "recv MSG_DONTWAIT: EAGAIN");

  // O_NONBLOCK and close-on-exec
  int flags = fcntl(sv[0], F_GETFL);
  check(flags >= 0 && (flags & O_ACCMODE) == O_RDWR && !(flags & O_NONBLOCK), "F_GETFL: O_RDWR, blocking");
  check(fcntl(sv[0], F_SETFL, flags | O_NONBLOCK) == 0, "F_SETFL O_NONBLOCK");
  check(fcntl(sv[0], F_GETFL) & O_NONBLOCK, "F_GETFL: O_NONBLOCK");
  check(FAILS_WITH(read(sv[0], buf, sizeof(buf)), EAGAIN), "nonblocking read: EAGAIN");
  check(fcntl(sv[0], F_GETFD) == 0, "F_GETFD: 0");
  check(fcntl(sv[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(sv[0], F_GETFD) == FD_CLOEXEC, "F_SETFD FD_CLOEXEC");

  // fill the buffer without blocking, then drain it
  long total = 0, n;
  memset(buf, 'z', sizeof(buf));
  while ((n = send(sv[0], buf, sizeof(buf), MSG_DONTWAIT)) > 0) total += n;
  check(n == -1 && errno == EAGAIN && total > 0, "send until full: EAGAIN");
  long drained = 0;
  while ((n = recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT)) > 0) drained += n;
  check(drained == total, "drain gets every byte");

  // a blocked read wakes up when the other end writes
  pid_t child = fork();
  if (child == 0) {
    usleep(100000);
    write(sv[0], "x", 1);
    _exit(0);
  }
  check(read(sv[1], buf, sizeof(buf)) == 1 && buf[0] == 'x', "blocking read wakes up");
  int status;
  waitpid(child, &status, 0);

  // options and names
  int value = 0;
  socklen_t len = sizeof(value);
  check(getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &value, &len) == 0 && value == SOCK_STREAM && len == sizeof(int),
        "SO_TYPE");
  value = 1;
  check(setsockopt(sv[0], SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value)) == 0, "set SO_KEEPALIVE");
  value = 0;
  len = sizeof(value);
  check(getsockopt(sv[0], SOL_SOCKET, SO_KEEPALIVE, &value, &len) == 0 && value == 1, "get SO_KEEPALIVE");
  struct sockaddr_storage name;
  len = sizeof(name);
  check(getsockname(sv[0], (struct sockaddr *)&name, &len) == 0 && len == sizeof(sa_family_t) &&
            name.ss_family == AF_UNIX,
        "getsockname: unnamed AF_UNIX");
  len = sizeof(name);
  check(getpeername(sv[0], (struct sockaddr *)&name, &len) == 0 && name.ss_family == AF_UNIX, "getpeername");

  // what a socketpair end can't do
  check(FAILS_WITH(send(1, "x", 1, 0), ENOTSOCK), "send on a non-socket: ENOTSOCK");
  struct sockaddr dest = {.sa_family = AF_UNIX};
  check(FAILS_WITH(sendto(sv[0], "x", 1, 0, &dest, sizeof(sa_family_t)), EISCONN), "sendto with an address: EISCONN");
  check(FAILS_WITH(listen(sv[0], 1), EINVAL), "listen: EINVAL");
  check(FAILS_WITH(accept(sv[0], NULL, NULL), EINVAL), "accept: EINVAL");

  // shutdown and close
  check(shutdown(sv[0], SHUT_WR) == 0, "shutdown SHUT_WR");
  check(read(sv[1], buf, sizeof(buf)) == 0, "peer reads end of stream");
  check(write(sv[1], "y", 1) == 1 && read(sv[0], buf, sizeof(buf)) == 1 && buf[0] == 'y', "other direction still works");
  check(FAILS_WITH(send(sv[0], "x", 1, MSG_NOSIGNAL), EPIPE), "send after SHUT_WR: EPIPE");
  check(close(sv[1]) == 0, "close one end");
  check(recv(sv[0], buf, sizeof(buf), 0) == 0, "recv after peer closed: end of stream");
  check(close(sv[0]) == 0, "close the other end");

  // flags at creation
  check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0, "socketpair NONBLOCK|CLOEXEC");
  check(FAILS_WITH(read(sv[0], buf, sizeof(buf)), EAGAIN), "SOCK_NONBLOCK read: EAGAIN");
  check(fcntl(sv[1], F_GETFD) == FD_CLOEXEC, "SOCK_CLOEXEC");
  close(sv[0]);
  close(sv[1]);

  puts(failures ? "some checks failed" : "done");
  return 0;
}
