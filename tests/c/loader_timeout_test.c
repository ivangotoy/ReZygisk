/* Host tests for the time limits of the loader's requests to zygiskd
   (loader/src/common/daemon.c with the real socket_utils.c). connect()
   is redirected to a stand-in daemon in the test dir that answers,
   closes, stays silent or never accepts. Built with a short
   ZYGISKD_PEER_TIMEOUT_MS. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int t_connect(int fd, const struct sockaddr *addr, socklen_t len);
#define connect(fd, a, l) t_connect(fd, a, l)

#include "../../loader/src/common/daemon.c"

#undef connect

#ifndef ZYGISKD_PEER_TIMEOUT_MS
#define ZYGISKD_PEER_TIMEOUT_MS 10000
#endif

/* relative to $TEST_DIR (cwd): the absolute path can exceed sun_path */
static int t_connect(int fd, const struct sockaddr *addr, socklen_t len) {
  (void)addr; (void)len;

  struct sockaddr_un a = { .sun_family = AF_UNIX };
  snprintf(a.sun_path, sizeof(a.sun_path), "daemon.sock");

  return connect(fd, (struct sockaddr *)&a, sizeof(a));
}

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  return ts.tv_sec + ts.tv_nsec / 1e9;
}

enum peer_mode { PEER_CLOSE, PEER_SILENT, PEER_FIRST_FIELD, PEER_NO_ACCEPT, PEER_COMPANION };

/* stand-in daemon in a child process; returns its pid */
static pid_t start_peer(enum peer_mode mode) {
  const char *dir = getenv("TEST_DIR");
  mkdir(dir, 0700);
  if (chdir(dir) == -1) return -1;
  unlink("daemon.sock");

  int ls = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = { .sun_family = AF_UNIX };
  snprintf(a.sun_path, sizeof(a.sun_path), "daemon.sock");
  if (bind(ls, (struct sockaddr *)&a, sizeof(a)) == -1 || listen(ls, 0) == -1) return -1;

  pid_t pid = fork();
  if (pid == 0) {
    alarm(10);
    if (mode == PEER_NO_ACCEPT) { pause(); _exit(0); }

    while (1) {
      int c = accept(ls, NULL, NULL);
      if (c == -1) _exit(0);
      uint8_t action = 0;
      read(c, &action, 1);

      if (mode == PEER_CLOSE) { close(c); continue; }
      if (mode == PEER_FIRST_FIELD) {
        uint32_t flags = 0;
        write(c, &flags, sizeof(flags));
      }
      if (mode == PEER_COMPANION) {
        size_t index = 0;
        read(c, &index, sizeof(index));
        uint8_t res = 1;
        write(c, &res, 1);
        usleep((ZYGISKD_PEER_TIMEOUT_MS * 2 + 100) * 1000);
        write(c, "x", 1);
      }
      pause();
    }
  }
  close(ls);

  return pid;
}

static void stop_peer(pid_t pid) {
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
}

/* request against <mode>: same result as against a closed peer, and done
   within about one timeout */
static int request(enum peer_mode mode) {
  alarm(5);

  pid_t closed = start_peer(PEER_CLOSE);
  uint32_t want = rezygiskd_get_process_flags(10123, "com.example.app");
  stop_peer(closed);

  pid_t peer = start_peer(mode);
  double start = now();
  uint32_t got = rezygiskd_get_process_flags(10123, "com.example.app");
  double took = now() - start;
  stop_peer(peer);

  double limit = 2.5 * ZYGISKD_PEER_TIMEOUT_MS / 1000.0;
  if (got == want && took < limit) return 0;

  printf("flags %u (closed peer: %u) after %.2f s (limit %.2f s)\n", got, want, took, limit);

  return 1;
}

/* GetInfo reads several fields: a peer that answers the first one and then
   goes silent ends the request after one timeout, not one per field */
static int test_first_field_then_silent(void) {
  alarm(5);

  pid_t peer = start_peer(PEER_FIRST_FIELD);
  struct rezygisk_info info;
  double start = now();
  rezygiskd_get_info(&info);
  double took = now() - start;
  stop_peer(peer);

  double limit = 2.5 * ZYGISKD_PEER_TIMEOUT_MS / 1000.0;
  if (took < limit) return 0;

  printf("GetInfo took %.2f s (limit %.2f s)\n", took, limit);

  return 1;
}

/* the fd given to a module companion carries no timeout */
static int test_companion_fd_untimed(void) {
  alarm(5);

  pid_t peer = start_peer(PEER_COMPANION);
  int fd = rezygiskd_connect_companion(0);
  if (fd == -1) { stop_peer(peer); return 1; }

  struct timeval rcv = { 0 }, snd = { 0 };
  socklen_t len = sizeof(rcv);
  getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, &len);
  len = sizeof(snd);
  getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, &len);

  char c = 0;
  ssize_t got = read(fd, &c, 1);
  stop_peer(peer);

  if (!rcv.tv_sec && !rcv.tv_usec && !snd.tv_sec && !snd.tv_usec && got == 1 && c == 'x') return 0;

  printf("timeouts rcv %ld.%06ld snd %ld.%06ld, late byte read %zd\n", (long)rcv.tv_sec, (long)rcv.tv_usec, (long)snd.tv_sec, (long)snd.tv_usec, got);

  return 1;
}

/* the daemon never accepts and its backlog is full: connect() itself must
   give up within about one timeout, without stacking its retries */
static int test_backlog_full(void) {
  alarm(5);

  pid_t peer = start_peer(PEER_NO_ACCEPT);
  int fill[4];
  for (int i = 0; i < 4; i++) {
    fill[i] = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "daemon.sock");
    connect(fill[i], (struct sockaddr *)&a, sizeof(a));
  }

  double start = now();
  bool ok = rezygiskd_zygote_injected();
  double took = now() - start;

  for (int i = 0; i < 4; i++) close(fill[i]);
  stop_peer(peer);

  double limit = 2.5 * ZYGISKD_PEER_TIMEOUT_MS / 1000.0;
  if (!ok && took < limit) return 0;

  printf("zygote_injected %d after %.2f s (limit %.2f s)\n", ok, took, limit);

  return 1;
}

/* guards: a non-blocking pipe still retries EAGAIN, a socket without a
   timeout still waits for its data */
static void *late_writer(void *arg) {
  usleep(100000);
  write(*(int *)arg, "abcd", 4);

  return NULL;
}

static int late_read(int rfd, int wfd) {
  alarm(5);

  pthread_t t;
  pthread_create(&t, NULL, late_writer, &wfd);
  char buf[4];
  ssize_t got = read_loop(rfd, buf, 4);
  pthread_join(t, NULL);

  return got == 4 && memcmp(buf, "abcd", 4) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);
  signal(SIGPIPE, SIG_IGN);

  if (strcmp(argv[1], "silent_peer") == 0) return request(PEER_SILENT);
  if (strcmp(argv[1], "first_field_then_silent") == 0) return test_first_field_then_silent();
  if (strcmp(argv[1], "companion_fd_untimed") == 0) return test_companion_fd_untimed();
  if (strcmp(argv[1], "backlog_full") == 0) return test_backlog_full();
  if (strcmp(argv[1], "nonblocking_pipe_retries") == 0) {
    int p[2];
    if (pipe(p) == -1) return 2;
    fcntl(p[0], F_SETFL, O_NONBLOCK);

    return late_read(p[0], p[1]);
  }
  if (strcmp(argv[1], "untimed_socket_waits") == 0) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) return 2;

    return late_read(sv[0], sv[1]);
  }

  return 2;
}
