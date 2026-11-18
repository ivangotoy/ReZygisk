/* Host test for the client loop of zygiskd/src/companion.c. The module
   library is not loaded: dlopen/dlsym return a stub entry. Linked with
   the real zygiskd/src/utils.c for the wire helpers. */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static void t_entry(int fd) { (void)fd; }

#define dlopen(p, f) ((void)(p), (void)(f), (void *)1)
#define dlsym(h, s) ((void)(h), (void)(s), (void *)t_entry)
#define dlclose(h) ((void)(h), 0)

#include "../../zygiskd/src/companion.c"

int __system_property_get(const char *name, char *value) { (void)name; value[0] = '\0'; return 0; }

static int send_client(int daemon_fd, int *peer) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) return -1;

  struct timeval tv = { .tv_sec = 5 };
  setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (write_fd(daemon_fd, sv[0]) == -1) {
    close(sv[0]);
    close(sv[1]);

    return -1;
  }
  close(sv[0]);
  *peer = sv[1];

  return 0;
}

/* a client gone before its acknowledgement must not end the companion */
static int test_survives_gone_client(void) {
  signal(SIGPIPE, SIG_IGN);

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) return 2;

  pid_t companion = fork();
  if (companion == -1) return 2;
  if (companion == 0) {
    alarm(20);
    close(sv[0]);
    companion_entry(sv[1]);

    _exit(0);
  }
  close(sv[1]);

  int result = 1;
  int lib = open("/dev/null", O_RDONLY);
  uint8_t ack = 0;
  if (write_string(sv[0], "example") == -1 || write_fd(sv[0], lib) == -1 ||
      read(sv[0], &ack, 1) != 1 || ack != 1) {
    printf("companion did not accept the module\n");

    goto out;
  }

  int gone = -1;
  if (send_client(sv[0], &gone) == -1) goto out;
  close(gone);

  usleep(200000);

  int peer = -1;
  ack = 0;
  if (send_client(sv[0], &peer) == -1 || read(peer, &ack, 1) != 1 || ack != 1) {
    printf("companion stopped serving after a client left before its acknowledgement\n");

    goto out;
  }

  result = 0;

  out:
    kill(companion, SIGKILL);
    waitpid(companion, NULL, 0);

    return result;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "survives_gone_client") == 0) return test_survives_gone_client();

  return 2;
}
