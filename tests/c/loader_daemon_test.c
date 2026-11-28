/* Host test for loader/src/common/daemon.c with ReZygiskd not running
   (its socket path does not exist on the host). Linked with the real
   loader/src/common/socket_utils.c. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* with fake_daemon set, the daemon socket is rz/<name> in the current dir
   and /data/adb/modules/ is $TEST_DIR/modules/ */
static bool fake_daemon = false;
static int t_connect(int fd, const struct sockaddr *addr, socklen_t len);
static FILE *t_fopen(const char *path, const char *mode);
#define connect(fd, a, l) t_connect(fd, a, l)
#define fopen(p, m) t_fopen(p, m)

#include "../../loader/src/common/daemon.c"

#undef connect
#undef fopen

static int t_connect(int fd, const struct sockaddr *addr, socklen_t len) {
  if (!fake_daemon) return connect(fd, addr, len);

  struct sockaddr_un mapped = { .sun_family = AF_UNIX };
  snprintf(mapped.sun_path, sizeof(mapped.sun_path), "rz/%s", strrchr(((const struct sockaddr_un *)addr)->sun_path, '/') + 1);

  return connect(fd, (struct sockaddr *)&mapped, sizeof(mapped));
}

static FILE *t_fopen(const char *path, const char *mode) {
  const char *prefix = "/data/adb/modules/";
  if (!fake_daemon || strncmp(path, prefix, strlen(prefix)) != 0) return fopen(path, mode);

  char buf[PATH_MAX];
  snprintf(buf, sizeof(buf), "%s/modules/%s", getenv("TEST_DIR"), path + strlen(prefix));

  return fopen(buf, mode);
}

/* zygisk-ptrace info: a caller's uninitialized struct must come back
   empty and safe to free when the daemon cannot be reached */
static int test_get_info_daemon_down(void) {
  if (access("/data/adb/rezygisk/" LP_SELECT("cp32", "cp64") ".sock", F_OK) == 0) return 2;

  struct rezygisk_info info;
  memset(&info, 0x5a, sizeof(info));

  rezygiskd_get_info(&info);

  if (info.running || info.modules.modules_count != 0 || info.modules.modules != NULL) {
    printf("running %d modules_count %zu modules %p\n", info.running, info.modules.modules_count, (void *)info.modules.modules);

    return 1;
  }

  free_rezygisk_info(&info);

  return 0;
}

/* GetInfo against a stand-in ReZygiskd listing one module whose
   module.prop ends with "name=<name>" and no newline */
static int get_info_name_at_eof(size_t name_len) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  char path[PATH_MAX];
  mkdir(dir, 0700);
  if (chdir(dir) == -1) return 2;
  mkdir("rz", 0700);
  mkdir("modules", 0700);
  mkdir("modules/example", 0700);

  char *name = malloc(name_len + 1);
  if (name == NULL) return 2;
  memset(name, 'n', name_len);
  name[name_len] = '\0';
  snprintf(path, sizeof(path), "%s/modules/example/module.prop", dir);
  FILE *f = (fopen)(path, "w");
  if (f == NULL) return 2;
  fprintf(f, "id=example\nname=%s", name);
  fclose(f);

  const char *sock = "rz/" SOCKET_FILE_NAME;
  unlink(sock);
  int srv = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
  if (srv == -1 || bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == -1 || listen(srv, 1) == -1) return 2;

  pid_t pid = fork();
  if (pid == 0) {
    alarm(5);
    int c = accept(srv, NULL, NULL);
    uint8_t action = 0;
    if (c == -1 || read(c, &action, 1) != 1) _exit(1);
    uint32_t flags = 1 << 30, dpid = 1234;
    size_t count = 1, len = strlen("example");
    write(c, &flags, 4); write(c, &dpid, 4); write(c, &count, sizeof(count));
    write(c, &len, sizeof(len)); write(c, "example", len);
    close(c);
    _exit(0);
  }
  close(srv);

  fake_daemon = true;
  struct rezygisk_info info;
  rezygiskd_get_info(&info);
  fake_daemon = false;
  waitpid(pid, NULL, 0);

  int result;
  if (name_len < 1000) {
    /* a whole name ended by the end of the file */
    result = info.modules.modules_count == 1 && info.modules.modules[0] && strcmp(info.modules.modules[0], name) == 0 ? 0 : 1;
  } else {
    /* longer than the 1023-byte line buffer: still refused */
    result = info.running && info.modules.modules_count == 0 ? 0 : 1;
  }
  if (result) printf("running %d, %zu module(s), first: %.40s\n", info.running, info.modules.modules_count, info.modules.modules_count ? info.modules.modules[0] : "(none)");

  free_rezygisk_info(&info);
  free(name);

  return result;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "get_info_daemon_down") == 0) return test_get_info_daemon_down();
  if (strcmp(argv[1], "get_info_name_at_eof") == 0) return get_info_name_at_eof(strlen("Example Module"));
  if (strcmp(argv[1], "get_info_name_too_long") == 0) return get_info_name_at_eof(1100);

  return 2;
}
