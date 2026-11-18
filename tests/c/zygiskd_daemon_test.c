/* Host test for the request loop of zygiskd/src/zygiskd.c. The daemon
   socket is created in $TEST_DIR, the controller socket and SELinux
   calls are no-ops, and the root implementation is a stub "Magisk".
   Linked with the real zygiskd/src/utils.c for the wire helpers. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define unix_listener_from_path t_listener
#define unix_datagram_sendto t_sendto
#define set_socket_create_context t_set_context
#define non_blocking_execv t_execv

/* module dir: /data/adb/modules is mapped to $TEST_DIR/modules */
#include <dirent.h>
static const char *t_map(const char *path, char *buf, size_t size);
static DIR *t_opendir(const char *path);
static int t_access(const char *path, int mode);
static int t_open(const char *path, int flags, ...);
#define opendir(p) t_opendir(p)
#define access(p, m) t_access(p, m)
#define open(p, ...) t_open(p, __VA_ARGS__)

/* accept(): the first call can be made to fail with a chosen errno */
static int inject_accept_errno = 0;
static bool break_stdout = false;
static int t_accept(int fd, struct sockaddr *addr, socklen_t *len);
#define accept(fd, a, l) t_accept(fd, a, l)

#include "../../zygiskd/src/zygiskd.c"

#undef accept
#undef opendir
#undef access
#undef open

static const char *t_map(const char *path, char *buf, size_t size) {
  const char *prefix = "/data/adb/modules";
  if (strncmp(path, prefix, strlen(prefix)) != 0) return path;

  snprintf(buf, size, "%s/modules%s", getenv("TEST_DIR"), path + strlen(prefix));

  return buf;
}

static DIR *t_opendir(const char *path) { char b[PATH_MAX]; return opendir(t_map(path, b, sizeof(b))); }
static int t_access(const char *path, int mode) { char b[PATH_MAX]; return access(t_map(path, b, sizeof(b)), mode); }
static int t_open(const char *path, int flags, ...) { char b[PATH_MAX]; return open(t_map(path, b, sizeof(b)), flags); }

/* Stand-in for the companion process: handshake like companion_entry(),
   then for each client fd report through it whether that fd blocks
   without a timeout (ack 1) or not (ack 2 or 3). */
/* when set, the companion never answers its handshake */
static bool stall_companion = false;

static void fake_companion(int fd) {
  if (stall_companion) sleep(30);

  char name[257];
  if (read_string(fd, name, sizeof(name)) == -1) _exit(1);
  int lib = read_fd(fd);
  if (lib == -1) _exit(1);
  close(lib);
  if (write_uint8_t(fd, 1) != 1) _exit(1);

  while (1) {
    int client = read_fd(fd);
    if (client == -1) _exit(0);

    struct timeval rcv = { 0 }, snd = { 0 };
    socklen_t len = sizeof(rcv);
    getsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &rcv, &len);
    len = sizeof(snd);
    getsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &snd, &len);

    /* the module keeps the channel: a byte sent later than any timeout must still arrive */
    char c = 0;
    uint8_t ack = 1;
    if (rcv.tv_sec || rcv.tv_usec || snd.tv_sec || snd.tv_usec) ack = 2;
    else if (read(client, &c, 1) != 1 || c != 'x') ack = 3;
    write(client, &ack, 1);
    close(client);
  }
}

/* when set, the process spawning the companion hangs instead (its pid is
   written to $TEST_DIR/spawn.pid) */
static bool hang_spawn = false;

int t_execv(const char *file, char *const argv[]) {
  (void)file;

  if (hang_spawn) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/spawn.pid", getenv("TEST_DIR"));
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
    sleep(30);
  }

  pid_t pid = fork();
  if (pid == -1) return -1;
  if (pid == 0) {
    fake_companion(atoi(argv[2]));

    _exit(0);
  }

  return 0;
}

static int t_accept(int fd, struct sockaddr *addr, socklen_t *len) {
  if (inject_accept_errno != 0) {
    errno = inject_accept_errno;
    inject_accept_errno = 0;

    return -1;
  }

  return accept(fd, addr, len);
}

static char sock_path[PATH_MAX];

int t_listener(const char *path) {
  (void)path;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) return -1;

  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1 || listen(fd, 2) == -1) {
    close(fd);

    return -1;
  }

  return fd;
}

void t_sendto(const char *path, const void *buf, size_t len) { (void)path; (void)buf; (void)len; }
void t_set_context(const char *context) { (void)context; }

int __system_property_get(const char *name, char *value) { (void)name; value[0] = '\0'; return 0; }

void get_impl(struct root_impl *uimpl) { uimpl->impl = Magisk; uimpl->variant = 0; }
bool uid_granted_root(uid_t uid) { (void)uid; return false; }
bool uid_should_umount(uid_t uid, const char *const process) { (void)uid; (void)process; return false; }
bool uid_is_manager(uid_t uid) { (void)uid; return false; }
void root_impl_cleanup(void) {}

static int connect_daemon(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) return -1;

  struct timeval tv = { .tv_sec = 5 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    close(fd);

    return -1;
  }

  return fd;
}

/* GetInfo must answer with Magisk flags, the daemon pid and 0 modules */
static int get_info_ok(pid_t daemon) {
  int fd = connect_daemon();
  if (fd == -1) {
    printf("connect: %s\n", strerror(errno));

    return 0;
  }

  uint8_t action = GetInfo;
  uint32_t flags = 0, pid = 0;
  size_t count = 1;
  int ok = write(fd, &action, 1) == 1 &&
           read(fd, &flags, sizeof(flags)) == (ssize_t)sizeof(flags) &&
           read(fd, &pid, sizeof(pid)) == (ssize_t)sizeof(pid) &&
           read(fd, &count, sizeof(count)) == (ssize_t)sizeof(count) &&
           flags == PROCESS_ROOT_IS_MAGISK && pid == (uint32_t)daemon && count == 0;
  if (!ok) printf("GetInfo reply: flags %u pid %u count %zu\n", flags, pid, count);

  close(fd);

  return ok;
}

/* a client that connects and leaves without a request must not stop the daemon */
static int test_survives_empty_client(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  mkdir(dir, 0700);
  /* relative path: $TEST_DIR can exceed sun_path */
  if (chdir(dir) == -1) return -1;
  snprintf(sock_path, sizeof(sock_path), "cp.sock");
  unlink(sock_path);

  pid_t daemon = fork();
  if (daemon == -1) return 2;
  if (daemon == 0) {
    alarm(20);

    char *argv[] = { "zygiskd", NULL };
    zygiskd_start(argv);

    _exit(0);
  }

  int result = 1;
  for (int i = 0; i < 100 && access(sock_path, F_OK) != 0; i++) usleep(50000);

  if (!get_info_ok(daemon)) {
    printf("daemon did not answer the first request\n");

    goto out;
  }

  int fd = connect_daemon();
  if (fd == -1) goto out;
  close(fd);

  usleep(200000);

  if (!get_info_ok(daemon)) {
    printf("daemon stopped serving after a client left without a request\n");

    goto out;
  }

  result = 0;

  out:
    kill(daemon, SIGKILL);
    waitpid(daemon, NULL, 0);

    return result;
}

static pid_t start_daemon(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return -1;
  mkdir(dir, 0700);
  /* relative path: $TEST_DIR can exceed sun_path */
  if (chdir(dir) == -1) return -1;
  snprintf(sock_path, sizeof(sock_path), "cp.sock");
  unlink(sock_path);

  pid_t daemon = fork();
  if (daemon == 0) {
    alarm(20);

    /* stdout open read-only: every log printf fails and sets errno */
    if (break_stdout) {
      int ro = open("/dev/null", O_RDONLY);
      dup2(ro, STDOUT_FILENO);
    }

    char *argv[] = { "zygiskd", NULL };
    zygiskd_start(argv);

    _exit(0);
  }

  for (int i = 0; i < 100 && access(sock_path, F_OK) != 0; i++) usleep(50000);

  return daemon;
}

/* accept() failing with <err> first: transient errors keep the daemon
   serving, others still end it */
static int accept_error(int err, bool should_survive) {
  inject_accept_errno = err;

  pid_t daemon = start_daemon();
  if (daemon == -1) return 2;

  int result = 1;
  if (should_survive) {
    /* the injected failure happens on the first accept, before this request */
    if (get_info_ok(daemon)) result = 0;
    else printf("daemon stopped after accept() failed with %s\n", strerror(err));
  } else {
    int status = 0;
    pid_t done = 0;
    for (int i = 0; i < 100 && (done = waitpid(daemon, &status, WNOHANG)) == 0; i++) usleep(50000);
    if (done == daemon) return 0;

    printf("daemon kept running after accept() failed with %s\n", strerror(err));
  }

  kill(daemon, SIGKILL);
  waitpid(daemon, NULL, 0);

  return result;
}

#ifndef ZYGISKD_PEER_TIMEOUT_MS
#define ZYGISKD_PEER_TIMEOUT_MS 10000
#endif

/* a client stuck before (or in the middle of) its request is dropped
   after the timeout, and the next client is served */
static int stalled_client(bool mid_request) {
  pid_t daemon = start_daemon();
  if (daemon == -1) return 2;

  int result = 1;
  int stuck = connect_daemon();
  if (stuck == -1) goto out;
  if (mid_request) {
    uint8_t action = GetProcessFlags;
    if (write(stuck, &action, 1) != 1) goto out;
  }

  if (get_info_ok(daemon)) result = 0;
  else printf("daemon did not serve the next client while one was stuck\n");

  close(stuck);

  out:
    kill(daemon, SIGKILL);
    waitpid(daemon, NULL, 0);

    return result;
}

/* a companion that never answers its handshake: the request fails like a
   failed spawn (answer 0), within about one timeout */
static int companion_handshake_stalls(void) {
  alarm(5);
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/modules", dir); mkdir(dir, 0700); mkdir(path, 0700);
  snprintf(path, sizeof(path), "%s/modules/example", dir); mkdir(path, 0700);
  snprintf(path, sizeof(path), "%s/modules/example/zygisk", dir); mkdir(path, 0700);
  snprintf(path, sizeof(path), "%s/modules/example/zygisk/" ARCH_STR ".so", dir);
  FILE *f = fopen(path, "w");
  if (f == NULL) return 2;
  fclose(f);

  stall_companion = true;
  pid_t daemon = start_daemon();
  if (daemon == -1) return 2;

  int result = 1;
  int fd = connect_daemon();
  if (fd == -1) goto out;

  uint8_t action = RequestCompanionSocket;
  size_t index = 0;
  if (write(fd, &action, 1) != 1 || write(fd, &index, sizeof(index)) != (ssize_t)sizeof(index)) goto out;

  uint8_t ack = 9;
  if (read(fd, &ack, 1) == 1 && ack == 0) result = 0;
  else printf("answer %u, expected 0\n", ack);
  close(fd);

  out:
    kill(daemon, SIGKILL);
    waitpid(daemon, NULL, 0);

    return result;
}

/* a companion spawn that never finishes: the request fails like a spawn
   that failed (answer 0) within about one timeout, and the stuck process is gone */
static int companion_spawn_hangs(void) {
  alarm(5);
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/modules", dir); mkdir(dir, 0700); mkdir(path, 0700);
  snprintf(path, sizeof(path), "%s/modules/example", dir); mkdir(path, 0700);
  snprintf(path, sizeof(path), "%s/modules/example/zygisk", dir); mkdir(path, 0700);
  snprintf(path, sizeof(path), "%s/modules/example/zygisk/" ARCH_STR ".so", dir);
  FILE *f = fopen(path, "w");
  if (f == NULL) return 2;
  fclose(f);
  snprintf(path, sizeof(path), "%s/spawn.pid", dir);
  unlink(path);

  hang_spawn = true;
  pid_t daemon = start_daemon();
  if (daemon == -1) return 2;

  int result = 1;
  int fd = connect_daemon();
  if (fd == -1) goto out;

  uint8_t action = RequestCompanionSocket;
  size_t index = 0;
  if (write(fd, &action, 1) != 1 || write(fd, &index, sizeof(index)) != (ssize_t)sizeof(index)) goto out;

  uint8_t ack = 9;
  if (read(fd, &ack, 1) != 1 || ack != 0) { printf("answer %u, expected 0\n", ack); goto out; }
  close(fd);

  f = fopen(path, "r");
  int pid = 0;
  if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
  if (pid > 0 && kill(pid, 0) == -1 && errno == ESRCH) result = 0;
  else printf("spawning process %d still exists\n", pid);

  out:
    kill(daemon, SIGKILL);
    waitpid(daemon, NULL, 0);

    return result;
}

/* the client fd handed to a module companion carries no timeout */
static int companion_channel_untimed(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/modules", dir); mkdir(dir, 0700); mkdir(path, 0700);
  snprintf(path, sizeof(path), "%s/modules/example", dir); mkdir(path, 0700);
  snprintf(path, sizeof(path), "%s/modules/example/zygisk", dir); mkdir(path, 0700);
  snprintf(path, sizeof(path), "%s/modules/example/zygisk/" ARCH_STR ".so", dir);
  FILE *f = fopen(path, "w");
  if (f == NULL) return 2;
  fclose(f);

  pid_t daemon = start_daemon();
  if (daemon == -1) return 2;

  int result = 1;
  int fd = connect_daemon();
  if (fd == -1) goto out;

  uint8_t action = RequestCompanionSocket;
  size_t index = 0;
  if (write(fd, &action, 1) != 1 || write(fd, &index, sizeof(index)) != (ssize_t)sizeof(index)) goto out;

  /* longer than the timeout of this build */
  usleep((ZYGISKD_PEER_TIMEOUT_MS * 2 + 100) * 1000);
  if (write(fd, "x", 1) != 1) goto out;

  uint8_t ack = 0;
  if (read(fd, &ack, 1) == 1 && ack == 1) result = 0;
  else printf("companion ack %u (2: fd carries a timeout, 3: channel closed by a timeout)\n", ack);

  close(fd);

  out:
    kill(daemon, SIGKILL);
    waitpid(daemon, NULL, 0);

    return result;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "survives_empty_client") == 0) return test_survives_empty_client();
  if (strcmp(argv[1], "accept_econnaborted") == 0) return accept_error(ECONNABORTED, true);
  if (strcmp(argv[1], "accept_eintr") == 0) return accept_error(EINTR, true);
  if (strcmp(argv[1], "accept_emfile_exits") == 0) return accept_error(EMFILE, false);
  if (strcmp(argv[1], "accept_enfile_exits") == 0) return accept_error(ENFILE, false);
  if (strcmp(argv[1], "accept_einval_exits") == 0) return accept_error(EINVAL, false);
  if (strcmp(argv[1], "stalled_client_before_request") == 0) return stalled_client(false);
  if (strcmp(argv[1], "stalled_client_mid_request") == 0) return stalled_client(true);
  if (strcmp(argv[1], "companion_channel_untimed") == 0) return companion_channel_untimed();
  if (strcmp(argv[1], "companion_spawn_hangs") == 0) return companion_spawn_hangs();
  if (strcmp(argv[1], "companion_handshake_stalls") == 0) return companion_handshake_stalls();
  if (strcmp(argv[1], "accept_econnaborted_log_fails") == 0) {
    break_stdout = true;

    return accept_error(ECONNABORTED, true);
  }

  return 2;
}
