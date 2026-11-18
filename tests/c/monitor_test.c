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
#include <sys/ptrace.h>
#include <sys/resource.h>
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
static int t_rename(const char *from, const char *to);
static int t_remove(const char *path);
static int t_stat(const char *path, struct stat *st);
#define bind(fd, a, l) t_bind(fd, a, l)
#define unlink(p) t_unlink(p)
#define rename(a, b) t_rename(a, b)
#define remove(p) t_remove(p)
#define stat(p, s) t_stat(p, s)
static pid_t t_waitpid(pid_t pid, int *status, int options);
#define waitpid(p, s, o) t_waitpid(p, s, o)
/* ptrace: never touches a real process; PTRACE_SEIZE can be made to fail */
static bool fail_seize = false;
static int seize_calls = 0;
static long t_ptrace(int request, ...);
#define ptrace(...) t_ptrace(__VA_ARGS__)
/* malloc: the largest request is recorded */
static size_t max_malloc = 0;
static void *t_malloc(size_t size);
#define malloc(n) t_malloc(n)

#include "../../loader/src/ptracer/monitor.c"

#undef bind
#undef unlink
#undef rename
#undef remove
#undef stat
#undef waitpid
#undef malloc
#undef ptrace

static long t_ptrace(int request, ...) {
  if (request == PTRACE_SEIZE) {
    seize_calls++;
    if (fail_seize) {
      errno = EPERM;

      return -1;
    }
  }

  return 0;
}

static void *t_malloc(size_t size) {
  if (size > max_malloc) max_malloc = size;

  return malloc(size);
}

/* waitpid(): reports ECHILD (no child left) when echild_mode is set,
   and ends the process if it is called over and over */
static bool echild_mode = false;
static int waitpid_calls = 0;
/* when set, the first waitpid() reports this pid as exited with status 1 */
static pid_t exited_pid = 0;
static int exited_status = 1 << 8;

static pid_t t_waitpid(pid_t pid, int *status, int options) {
  if (exited_pid != 0) {
    pid_t ret = exited_pid;
    exited_pid = 0;
    *status = exited_status;

    return ret;
  }
  if (!echild_mode) return waitpid(pid, status, options);

  if (++waitpid_calls > 1000) {
    printf("waitpid called %d times without a child: busy loop\n", waitpid_calls);

    _exit(1);
  }

  errno = ECHILD;

  return -1;
}

/* /data/adb/modules/rezygisk/<f> is mapped to $TEST_DIR/<f> */
static const char *t_map(const char *path, char *buf, size_t size) {
  const char *prefix = "/data/adb/rezygisk/";
  const char *modprefix = "/data/adb/modules/rezygisk/";
  if (strncmp(path, modprefix, strlen(modprefix)) == 0) {
    snprintf(buf, size, "%s/%s", getenv("TEST_DIR"), path + strlen(modprefix));

    return buf;
  }
  if (strncmp(path, prefix, strlen(prefix)) != 0) return path;

  snprintf(buf, size, "%s/rz/%s", getenv("TEST_DIR"), path + strlen(prefix));

  return buf;
}

static int t_rename(const char *from, const char *to) {
  char a[PATH_MAX], b[PATH_MAX];

  return rename(t_map(from, a, sizeof(a)), t_map(to, b, sizeof(b)));
}

static int t_remove(const char *path) {
  char buf[PATH_MAX];

  return remove(t_map(path, buf, sizeof(buf)));
}

static int t_stat(const char *path, struct stat *st) {
  char buf[PATH_MAX];

  return stat(t_map(path, buf, sizeof(buf)), st);
}

static int t_bind(int fd, const struct sockaddr *addr, socklen_t len) {
  (void)len;

  /* relative to $TEST_DIR (the listener tests chdir there): the absolute
     path can be longer than sun_path */
  struct sockaddr_un mapped = { .sun_family = AF_UNIX };
  const char *path = ((const struct sockaddr_un *)addr)->sun_path;
  snprintf(mapped.sun_path, sizeof(mapped.sun_path), "rz/%s", strrchr(path, '/') + 1);

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
  char buf[PATH_MAX];
  path = t_map(path, buf, sizeof(buf));

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
  snprintf(state_path, sizeof(state_path), "%s/rz", dir);
  mkdir(state_path, 0700);
  snprintf(state_path, sizeof(state_path), "%s/rz/state.json", dir);
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

static int make_test_dir_only(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return -1;
  mkdir(dir, 0700);

  return 0;
}

static int write_prop(const char *content) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return -1;
  mkdir(dir, 0700);
  snprintf(prop_path, sizeof(prop_path), "%s/module.prop", dir);
  snprintf(state_path, sizeof(state_path), "%s/rz", dir);
  mkdir(state_path, 0700);
  snprintf(state_path, sizeof(state_path), "%s/rz/state.json", dir);

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

static int read_file(const char *path, char *buf, size_t size) {
  FILE *f = (fopen)(path, "r");
  if (f == NULL) return -1;
  size_t n = fread(buf, 1, size - 1, f);
  buf[n] = '\0';
  (fclose)(f);

  return 0;
}

#define PROP_FIXTURE \
  "id=rezygisk\nname=ReZygisk\nversion=v1.0.0 (1-abcdef0-release)\nversionCode=1\n" \
  "author=The PerformanC Organization\ndescription=Standalone implementation of Zygisk.\n" \
  "updateJson=https://example.invalid/update.json\nmanagedFeatures=kernel_umount\n"

/* normal rewrite: only the description changes, the mode is kept */
static int test_prop_rewrite_exact(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;
  if (chmod(prop_path, 0640) == -1) return 2;

  if (!prepare_environment()) return 2;
  tracing_state = TRACING;
  status64.supported = true;
  status64.daemon_running = true;
  status64.zygote_injected = true;

  if (!update_status(NULL)) {
    printf("update_status failed\n");

    return 1;
  }

  char got[4096];
  if (read_file(prop_path, got, sizeof(got)) == -1) return 2;

  const char *want =
    "id=rezygisk\nname=ReZygisk\nversion=v1.0.0 (1-abcdef0-release)\nversionCode=1\n"
    "author=The PerformanC Organization\ndescription=[Monitor: \u2705, ReZygisk 64-bit: \u2705] Standalone implementation of Zygisk.\n"
    "updateJson=https://example.invalid/update.json\nmanagedFeatures=kernel_umount\n";
  if (strcmp(got, want) != 0) {
    printf("module.prop:\n%s\n", got);

    return 1;
  }

  struct stat st;
  if (stat(prop_path, &st) == -1) return 2;
  if ((st.st_mode & 07777) != 0640) {
    printf("mode %o, expected 640\n", (unsigned)(st.st_mode & 07777));

    return 1;
  }

  return 0;
}

/* a write that fails part way (file size limit, like a full disk) must
   leave the old module.prop as it was */
static int test_prop_write_failure_keeps_old(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;

  pid_t pid = fork();
  if (pid == 0) {
    if (!prepare_environment()) _exit(2);
    status64.supported = true;

    signal(SIGXFSZ, SIG_IGN);
    struct rlimit lim = { .rlim_cur = 100, .rlim_max = 100 };
    if (setrlimit(RLIMIT_FSIZE, &lim) == -1) _exit(2);

    update_status(NULL);

    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 2;

  char got[4096];
  if (read_file(prop_path, got, sizeof(got)) == -1) return 2;
  if (strcmp(got, PROP_FIXTURE) == 0) return 0;

  printf("module.prop after the failed write (%zu bytes):\n%s\n", strlen(got), got);

  return 1;
}

/* a long daemon error text must not overflow the 256-byte status text */
static int test_status_text_bounded(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;

  pid_t pid = fork();
  if (pid == 0) {
    if (!prepare_environment()) _exit(2);

    static char error_info[401];
    memset(error_info, 'e', 400);
    status64.supported = true;
    status64.daemon_running = false;
    status64.daemon_error_info = error_info;

    update_status(NULL);

    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    printf("child status 0x%x\n", status);

    return 1;
  }

  char got[4096];
  if (read_file(prop_path, got, sizeof(got)) == -1) return 2;

  const char *start = strstr(got, "description=[");
  const char *end = start ? strstr(start, "] Standalone") : NULL;
  if (start == NULL || end == NULL) {
    printf("module.prop:\n%s\n", got);

    return 1;
  }

  size_t len = (size_t)(end - (start + strlen("description=[")));
  if (len <= 255) return 0;

  printf("status text is %zu bytes\n", len);

  return 1;
}

/* lines after the description that fill post_section exactly: the
   description newline still fits and no kept line is cut */
static int test_sections_exact_fit(void) {
  char content[4096], pad[977];
  const char *update = "updateJson=https://example.invalid/update.json\n";
  size_t pad_len = 1023 - strlen(update) - strlen("pad=") - 1;
  memset(pad, 'x', pad_len); pad[pad_len] = '\0';
  snprintf(content, sizeof(content), "id=rezygisk\ndescription=Standalone implementation of Zygisk.\n%spad=%s\n", update, pad);
  if (strlen(content) - strlen("id=rezygisk\ndescription=Standalone implementation of Zygisk.\n") != 1023) return 2;
  if (write_prop(content) == -1) return 2;

  pid_t pid = fork();
  if (pid == 0) {
    if (!prepare_environment()) _exit(2);

    size_t post = strlen(post_section);
    if (post >= sizeof(post_section) || post == 0 || post_section[post - 1] != '\n') {
      printf("post_section (%zu bytes) ends: %s\n", post, post > 40 ? post_section + post - 40 : post_section);

      _exit(1);
    }

    /* every line after the description is a whole line of the file */
    const char *rest = strchr(post_section, '\n') + 1;
    while (*rest) {
      const char *nl = strchr(rest, '\n');
      size_t len = (size_t)(nl - rest) + 1;
      if (len != strlen(update) || strncmp(rest, update, len) != 0) {
        if (!(strncmp(rest, "pad=", 4) == 0 && len == 4 + pad_len + 1)) {
          printf("cut line: %.40s\n", rest);

          _exit(1);
        }
      }
      rest = nl + 1;
    }

    _exit(strstr(post_section, update) ? 0 : 4);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

  printf("child status 0x%x\n", status);

  return 1;
}

/* strings in state.json are JSON-escaped */
static int test_state_json_escapes(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;
  if (!prepare_environment()) return 2;

  static char *modules[] = { "example_module" };
  environment_information64.root_impl = "Magisk";
  environment_information64.modules = modules;
  environment_information64.modules_len = 1;
  status64.supported = true;
  status64.daemon_running = false;
  status64.daemon_error_info = "exited: \"quoted\" back\\slash\ttab";

  update_status(NULL);

  char got[4096];
  if (read_file(state_path, got, sizeof(got)) == -1) return 2;

  const char *want = "\"reason\": \"exited: \\\"quoted\\\" back\\\\slash\\u0009tab\"";
  if (strstr(got, want) && strstr(got, "\"modules\": [\"example_module\"]")) return 0;

  printf("state.json:\n%s\nexpected to contain: %s\n", got, want);

  return 1;
}

/* a state.json write that fails part way keeps the old state.json */
static int test_state_json_write_failure_keeps_old(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;

  FILE *f = (fopen)(state_path, "w");
  if (f == NULL) return 2;
  fputs("{\"old\": true}\n", f);
  (fclose)(f);

  pid_t pid = fork();
  if (pid == 0) {
    if (!prepare_environment()) _exit(2);

    static char names[40][24];
    static char *modules[40];
    for (int i = 0; i < 40; i++) {
      snprintf(names[i], sizeof(names[i]), "example_module_%02d", i);
      modules[i] = names[i];
    }
    environment_information64.root_impl = "Magisk";
    environment_information64.modules = modules;
    environment_information64.modules_len = 40;
    status64.supported = true;
    status64.daemon_running = true;

    /* module.prop (about 330 bytes) fits, state.json (over 900) does not */
    signal(SIGXFSZ, SIG_IGN);
    struct rlimit lim = { .rlim_cur = 600, .rlim_max = 600 };
    if (setrlimit(RLIMIT_FSIZE, &lim) == -1) _exit(2);

    update_status(NULL);

    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 2;

  char got[4096];
  if (read_file(state_path, got, sizeof(got)) == -1) return 2;
  if (strcmp(got, "{\"old\": true}\n") == 0) return 0;

  printf("state.json after the failed write (%zu bytes): %.60s...\n", strlen(got), got);

  return 1;
}

/* SIGCHLD while tracing, but no child left to wait for: the handler must
   return instead of calling waitpid() forever */
static int test_sigchld_echild_tracing(void) {
  if (make_test_dir_only() == -1) return 2;

  pid_t pid = fork();
  if (pid == 0) {
    alarm(10);
    if (!sigchld_listener_init()) _exit(2);

    tracing_state = TRACING;
    echild_mode = true;
    kill(getpid(), SIGCHLD);

    sigchld_listener_callback();

    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

  printf("child status 0x%x\n", status);

  return 1;
}

/* ReZygiskd exiting must show up in module.prop right away */
static int test_daemon_exit_updates_status(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;

  pid_t pid = fork();
  if (pid == 0) {
    alarm(10);
    if (!prepare_environment()) _exit(2);
    if (!sigchld_listener_init()) _exit(2);

    tracing_state = TRACING;
    status64.supported = true;
    status64.daemon_running = true;
    status64.zygote_injected = true;
    status64.daemon_pid = 4242;

    exited_pid = 4242;
    echild_mode = true;
    kill(getpid(), SIGCHLD);

    sigchld_listener_callback();

    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    printf("child status 0x%x\n", status);

    return 1;
  }

  char got[4096];
  if (read_file(prop_path, got, sizeof(got)) == -1) return 2;
  if (strstr(got, "description=[Monitor: \u2705, ReZygisk 64-bit: \u26a0\ufe0f(ReZygiskd: test)] Standalone")) return 0;

  printf("module.prop after the daemon exited:\n%s\n", got);

  return 1;
}

/* an automatic stop (e.g. "Zygote crashed") must show up in module.prop
   once tracing of init has stopped */
static int test_auto_stop_updates_status(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;

  pid_t pid = fork();
  if (pid == 0) {
    alarm(10);
    if (!prepare_environment()) _exit(2);
    if (!sigchld_listener_init()) _exit(2);

    /* state left by the PRE_INJECT auto-stop */
    tracing_state = STOPPING;
    monitor_stop_reason = "Zygote crashed";
    status64.supported = true;
    status64.daemon_running = true;

    /* init reports the group-stop that PTRACE_INTERRUPT asked for */
    exited_pid = 1;
    exited_status = ((SIGTRAP | (PTRACE_EVENT_STOP << 8)) << 8) | 0x7f;
    echild_mode = true;
    kill(getpid(), SIGCHLD);

    sigchld_listener_callback();

    _exit(tracing_state == STOPPED ? 0 : 3);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    printf("child status 0x%x\n", status);

    return 1;
  }

  char got[4096];
  if (read_file(prop_path, got, sizeof(got)) == -1) return 2;
  if (strstr(got, "description=[Monitor: \u26d4, ReZygisk 64-bit: \u274c] Standalone")) return 0;

  printf("module.prop after the stop:\n%s\n", got);

  return 1;
}

/* a line break in the daemon error text must not add lines to module.prop */
static int test_status_no_line_injection(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;

  pid_t pid = fork();
  if (pid == 0) {
    if (!prepare_environment()) _exit(2);

    status64.supported = true;
    status64.daemon_running = false;
    status64.daemon_error_info = "boom\nupdateJson=https://other.invalid/update.json\r";

    update_status(NULL);

    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 2;

  char got[4096];
  if (read_file(prop_path, got, sizeof(got)) == -1) return 2;

  int lines = 0;
  for (const char *c = got; *c; c++) if (*c == '\n') lines++;
  if (lines == 8 && strstr(got, "\nupdateJson=https://other.invalid") == NULL && strchr(got, '\r') == NULL) return 0;

  printf("module.prop (%d lines):\n%s\n", lines, got);

  return 1;
}

/* lines before the description that nearly fill pre_section must not
   push out the "description=" key itself */
static int test_pre_section_keeps_key(void) {
  char content[4096], pad[1001];
  memset(pad, 'p', 1000); pad[1000] = '\0';
  /* "id=rezygisk\n" (12) + "pad=" + 1000 + "\n" = 1017 bytes before the description */
  snprintf(content, sizeof(content), "id=rezygisk\npad=%s\ndescription=Standalone implementation of Zygisk.\nupdateJson=https://example.invalid/update.json\n", pad);
  if (write_prop(content) == -1) return 2;

  pid_t pid = fork();
  if (pid == 0) {
    if (!prepare_environment()) _exit(2);

    size_t pre = strlen(pre_section);
    if (pre >= strlen("description=") && strcmp(pre_section + pre - strlen("description="), "description=") == 0) _exit(0);

    printf("pre_section (%zu bytes) ends: %s\n", pre, pre > 30 ? pre_section + pre - 30 : pre_section);

    _exit(1);
  }

  int status = 0;
  waitpid(pid, &status, 0);

  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

/* a daemon info datagram claiming more modules than it carries must not
   make the monitor allocate for that count */
static int test_info_module_count_bounded(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;
  if (!prepare_environment()) return 2;

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, sv) == -1) return 2;
  monitor_sock_fd = sv[0];

  char packet[sizeof(struct rzd_msg_header) + sizeof(struct rzd_info_payload) + 2];
  memset(packet, 0, sizeof(packet));
  struct rzd_msg_header *hdr = (struct rzd_msg_header *)packet;
  hdr->cmd = DAEMON64_SET_INFO;
  hdr->len = (uint32_t)(sizeof(packet) - sizeof(struct rzd_msg_header));
  struct rzd_info_payload *info = (struct rzd_info_payload *)hdr->data;
  memcpy(info->impl, "Magisk", 7);
  info->modules_count = 0xFFFFFFFFu;
  info->data[0] = 'a';
  if (send(sv[1], packet, sizeof(packet), 0) != (ssize_t)sizeof(packet)) return 2;

  max_malloc = 0;
  rezygiskd_listener_callback();

  if (max_malloc < 4096) return 0;

  printf("largest allocation %zu bytes for a 2-byte module list\n", max_malloc);

  return 1;
}

/* ctl start from STOPPED: tracing is on only if init was seized */
static int ctl_start(bool seize_fails) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;
  if (!prepare_environment()) return 2;

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, sv) == -1) return 2;
  monitor_sock_fd = sv[0];

  struct rzd_msg_header hdr = { .cmd = START, .len = 0 };
  if (send(sv[1], &hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) return 2;

  tracing_state = STOPPED;
  monitor_stop_reason = "user requested";
  fail_seize = seize_fails;
  rezygiskd_listener_callback();

  enum ptracer_tracing_state want = seize_fails ? STOPPED : TRACING;
  if (seize_calls != 1 || tracing_state != want) {
    printf("seize calls %d, state %d, expected %d\n", seize_calls, tracing_state, want);

    return 1;
  }

  /* tracing again: the reason of the last stop no longer applies */
  if (!seize_fails && monitor_stop_reason != NULL) {
    printf("stop reason \"%s\" kept while tracing\n", monitor_stop_reason);

    return 1;
  }

  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "state_json_stop_reason") == 0) return test_state_json_stop_reason();
  if (strcmp(argv[1], "listener_stale_path") == 0) return test_listener_stale_path();
  if (strcmp(argv[1], "sections_bounded") == 0) return test_sections_bounded();
  if (strcmp(argv[1], "sections_exact_fit") == 0) return test_sections_exact_fit();
  if (strcmp(argv[1], "pre_section_keeps_key") == 0) return test_pre_section_keeps_key();
  if (strcmp(argv[1], "prop_rewrite_exact") == 0) return test_prop_rewrite_exact();
  if (strcmp(argv[1], "prop_write_failure_keeps_old") == 0) return test_prop_write_failure_keeps_old();
  if (strcmp(argv[1], "status_text_bounded") == 0) return test_status_text_bounded();
  if (strcmp(argv[1], "state_json_escapes") == 0) return test_state_json_escapes();
  if (strcmp(argv[1], "state_json_write_failure_keeps_old") == 0) return test_state_json_write_failure_keeps_old();
  if (strcmp(argv[1], "sigchld_echild_tracing") == 0) return test_sigchld_echild_tracing();
  if (strcmp(argv[1], "daemon_exit_updates_status") == 0) return test_daemon_exit_updates_status();
  if (strcmp(argv[1], "auto_stop_updates_status") == 0) return test_auto_stop_updates_status();
  if (strcmp(argv[1], "status_no_line_injection") == 0) return test_status_no_line_injection();
  if (strcmp(argv[1], "info_module_count_bounded") == 0) return test_info_module_count_bounded();
  if (strcmp(argv[1], "ctl_start_seizes") == 0) return ctl_start(false);
  if (strcmp(argv[1], "ctl_start_seize_fails") == 0) return ctl_start(true);
  if (strcmp(argv[1], "listener_bind_fail_closes") == 0) return test_listener_bind_fail_closes();

  return 2;
}
