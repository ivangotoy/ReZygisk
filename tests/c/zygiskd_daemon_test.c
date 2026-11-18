/* Host test for the request loop of zygiskd/src/zygiskd.c. The daemon
   socket is created in $TEST_DIR, the controller socket and SELinux
   calls are no-ops, and the root implementation is a stub "Magisk".
   Linked with the real zygiskd/src/utils.c for the wire helpers. */
#include <errno.h>
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

#include "../../zygiskd/src/zygiskd.c"

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
  snprintf(sock_path, sizeof(sock_path), "%s/cp.sock", dir);
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

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "survives_empty_client") == 0) return test_survives_empty_client();

  return 2;
}
