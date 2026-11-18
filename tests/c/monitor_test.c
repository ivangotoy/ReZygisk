/* Host test for the state.json writer of loader/src/ptracer/monitor.c.
   module.prop and state.json are redirected to $TEST_DIR; bionic-only
   names used by ptracer/utils.h get host stand-ins. */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define sigabbrev_np t_sigabbrev_np
#define sys_signame t_sys_signame
static const char *t_sys_signame[65];

#ifndef ZKSU_VERSION
#define ZKSU_VERSION "test"
#endif

static FILE *t_fopen(const char *path, const char *mode);
#define fopen(p, m) t_fopen(p, m)

/* paths under /data/adb/rezygisk/ are mapped to rz/ relative to the
   current dir ($TEST_DIR in the listener tests), short enough for sun_path */
static const char *t_map(const char *path, char *buf, size_t size);
static int t_bind(int fd, const struct sockaddr *addr, socklen_t len);
static int t_unlink(const char *path);
#define bind(fd, a, l) t_bind(fd, a, l)
#define unlink(p) t_unlink(p)

#include "../../loader/src/ptracer/monitor.c"

#undef bind
#undef unlink

static const char *t_map(const char *path, char *buf, size_t size) {
  const char *prefix = "/data/adb/rezygisk/";
  if (strncmp(path, prefix, strlen(prefix)) != 0) return path;

  snprintf(buf, size, "rz/%s", path + strlen(prefix));

  return buf;
}

static int t_bind(int fd, const struct sockaddr *addr, socklen_t len) {
  (void)len;

  struct sockaddr_un mapped = { .sun_family = AF_UNIX };
  char buf[PATH_MAX];
  snprintf(mapped.sun_path, sizeof(mapped.sun_path), "%s", t_map(((const struct sockaddr_un *)addr)->sun_path, buf, sizeof(buf)));

  return bind(fd, (struct sockaddr *)&mapped, sizeof(mapped));
}

static int t_unlink(const char *path) {
  char buf[PATH_MAX];

  return unlink(t_map(path, buf, sizeof(buf)));
}

int fork_dont_care() { return -1; }
void parse_status(int status, char *buf, size_t len) { (void)status; snprintf(buf, len, "test"); }
int get_program(int pid, char *buf, size_t size) { (void)pid; (void)size; buf[0] = '\0'; return -1; }

static char state_path[PATH_MAX];
static char prop_path[PATH_MAX];

static FILE *t_fopen(const char *path, const char *mode) {
  if (strcmp(path, "/data/adb/rezygisk/state.json") == 0) path = state_path;
  else if (strcmp(path, "/data/adb/modules/rezygisk/module.prop") == 0) path = prop_path;

  return (fopen)(path, mode);
}

/* a ',' whose next non-blank character closes an object or array is a
   trailing comma, which JSON.parse rejects */
static int has_trailing_comma(const char *s) {
  for (const char *p = s; *p; p++) {
    if (*p != ',') continue;

    const char *q = p + 1;
    while (*q == ' ' || *q == '\n' || *q == '\t' || *q == '\r') q++;
    if (*q == '}' || *q == ']') return 1;
  }

  return 0;
}

static int test_state_json_stop_reason(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  mkdir(dir, 0700);
  snprintf(state_path, sizeof(state_path), "%s/state.json", dir);
  snprintf(prop_path, sizeof(prop_path), "%s/module.prop", dir);

  environment_information64.root_impl = "Magisk";
  tracing_state = STOPPED;
  monitor_stop_reason = "user requested";
  status64.supported = true;
  status64.daemon_running = true;
  status64.zygote_injected = true;

  if (!update_status(NULL)) return 2;

  FILE *f = (fopen)(state_path, "r");
  if (f == NULL) return 2;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = '\0';
  (fclose)(f);

  if (strstr(buf, "\"reason\": \"user requested\"") == NULL || has_trailing_comma(buf)) {
    printf("%s", buf);

    return 1;
  }

  return 0;
}

static int make_rz_dir(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return -1;
  mkdir(dir, 0700);

  char rz[PATH_MAX];
  snprintf(rz, sizeof(rz), "%s/rz", dir);
  if (mkdir(rz, 0700) == -1 && errno != EEXIST) return -1;

  return chdir(dir);
}

/* a socket file left by an earlier monitor must not block the bind */
static int test_listener_stale_path(void) {
  if (make_rz_dir() == -1) return 2;

  int fd = open("rz/" SOCKET_NAME, O_WRONLY | O_CREAT, 0600);
  if (fd == -1) return 2;
  close(fd);

  if (rezygiskd_listener_init()) return 0;

  printf("bind failed over a stale socket path\n");

  return 1;
}

/* a failed bind must not leave the socket open */
static int test_listener_bind_fail_closes(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  mkdir(dir, 0700);
  char empty[PATH_MAX];
  snprintf(empty, sizeof(empty), "%s/empty", dir);
  mkdir(empty, 0700);
  if (chdir(empty) == -1) return 2;

  /* no rz/ dir: bind fails with ENOENT */
  if (rezygiskd_listener_init()) return 2;

  if (monitor_sock_fd >= 0 && fcntl(monitor_sock_fd, F_GETFD) != -1) {
    printf("socket fd %d still open after the failed bind\n", monitor_sock_fd);

    return 1;
  }

  return 0;
}

static int write_prop(const char *content) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return -1;
  mkdir(dir, 0700);
  snprintf(prop_path, sizeof(prop_path), "%s/module.prop", dir);
  snprintf(state_path, sizeof(state_path), "%s/state.json", dir);

  FILE *f = (fopen)(prop_path, "w");
  if (f == NULL) return -1;
  fputs(content, f);

  return (fclose)(f) == 0 ? 0 : -1;
}

/* a module.prop larger than the section buffers must not overflow them;
   runs in a child so a crash is a failure too */
static int test_sections_bounded(void) {
  char content[4096], desc[1501];
  memset(desc, 'd', 1500); desc[1500] = '\0';
  snprintf(content, sizeof(content), "id=rezygisk\nname=ReZygisk\ndescription=%s\nupdateJson=https://example.invalid/update.json\n", desc);
  if (write_prop(content) == -1) return 2;

  pid_t pid = fork();
  if (pid == 0) {
    if (!prepare_environment()) _exit(3);

    size_t pre = strlen(pre_section), post = strlen(post_section);
    if (pre >= sizeof(pre_section) || post >= sizeof(post_section)) {
      printf("pre_section %zu, post_section %zu bytes in %zu-byte buffers\n", pre, post, sizeof(post_section));

      _exit(1);
    }

    /* the description is shortened, the line after it stays whole */
    const char *tail = "\nupdateJson=https://example.invalid/update.json\n";
    size_t tail_len = strlen(tail);
    if (post < tail_len || strcmp(post_section + post - tail_len, tail) != 0 || post_section[0] != 'd') {
      printf("post_section ends: %s\n", post > 80 ? post_section + post - 80 : post_section);

      _exit(4);
    }

    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

  printf("child status 0x%x\n", status);

  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "state_json_stop_reason") == 0) return test_state_json_stop_reason();
  if (strcmp(argv[1], "listener_stale_path") == 0) return test_listener_stale_path();
  if (strcmp(argv[1], "sections_bounded") == 0) return test_sections_bounded();
  if (strcmp(argv[1], "listener_bind_fail_closes") == 0) return test_listener_bind_fail_closes();

  return 2;
}
