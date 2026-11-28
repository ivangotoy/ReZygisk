/* Host test for the state.json writer of loader/src/ptracer/monitor.c.
   module.prop and state.json are redirected to $TEST_DIR; bionic-only
   names used by ptracer/utils.h get host stand-ins. */
#include <errno.h>
#include <stdarg.h>
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
static bool sigchld_on_seize = false;
static int seize_calls = 0;
static long t_ptrace(int request, ...);
#define ptrace(...) t_ptrace(__VA_ARGS__)
/* kill: recorded per signal, never sent */
static int signals_sent[65];
#define kill(p, sig) ((void)(p), signals_sent[(sig) & 63]++, 0)
/* sendto: socket paths under /data/adb/rezygisk/ go to rz/ in the cwd */
static ssize_t t_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t alen);
#define sendto(fd, b, l, f, a, al) t_sendto(fd, b, l, f, a, al)
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
#undef kill
#undef sendto

static bool detach_with_sigstop = false;
static int plain_detaches = 0;

static ssize_t t_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t alen) {
  (void)alen;
  struct sockaddr_un mapped = { .sun_family = AF_UNIX };
  const char *path = ((const struct sockaddr_un *)addr)->sun_path;
  snprintf(mapped.sun_path, sizeof(mapped.sun_path), "rz/%s", strrchr(path, '/') + 1);

  return sendto(fd, buf, len, flags, (struct sockaddr *)&mapped, sizeof(mapped));
}

static long t_ptrace(int request, ...) {
  if (request == PTRACE_DETACH) {
    va_list ap;
    va_start(ap, request);
    (void)va_arg(ap, pid_t);
    (void)va_arg(ap, void *);
    long sig = (long)va_arg(ap, void *);
    va_end(ap);
    if (sig == SIGSTOP) detach_with_sigstop = true;
    else plain_detaches++;
  }
  if (request == PTRACE_SEIZE) {
    seize_calls++;
    /* init forks right after it is seized: its fork stop is reported now */
    if (sigchld_on_seize) kill(getpid(), SIGCHLD);
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

static pid_t hang_pid = 0;

/* when set, the first waitpid() ends the process with exit code 0 */
static bool exit_on_waitpid = false;

static pid_t t_waitpid(pid_t pid, int *status, int options) {
  if (exit_on_waitpid) _exit(0);
  /* the process never reports its stop */
  if (hang_pid != 0 && pid == hang_pid) {
    if (options & WNOHANG) return 0;

    for (;;) pause();
  }
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
static const char *test_program = NULL;
int get_program(int pid, char *buf, size_t size) {
  (void)pid;
  if (test_program == NULL) { buf[0] = '\0'; return -1; }
  snprintf(buf, size, "%s", test_program);

  return 0;
}

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

/* the epoll fd is not inherited by the processes the monitor starts */
static int test_epoll_fd_cloexec(void) {
  if (!monitor_events_init()) return 2;

  int flags = fcntl(monitor_epoll_fd, F_GETFD);
  if (flags != -1 && (flags & FD_CLOEXEC)) return 0;

  printf("epoll fd %d flags %d: no FD_CLOEXEC\n", monitor_epoll_fd, flags);

  return 1;
}

/* every byte sequence is a whole UTF-8 character */
static int utf8_valid(const char *text) {
  const unsigned char *s = (const unsigned char *)text;
  while (*s) {
    int n = *s < 0x80 ? 0 : (*s & 0xE0) == 0xC0 ? 1 : (*s & 0xF0) == 0xE0 ? 2 : (*s & 0xF8) == 0xF0 ? 3 : -1;
    if (n < 0) return 0;
    s++;
    for (int i = 0; i < n; i++, s++) {
      if ((*s & 0xC0) != 0x80) return 0;
    }
  }

  return 1;
}

/* a daemon error text of two-byte characters cut by the status text
   limit: module.prop stays valid UTF-8 (both offsets, one of them puts
   a character across the limit) */
static int test_status_text_utf8_whole(void) {
  for (int shift = 0; shift < 2; shift++) {
    if (write_prop(PROP_FIXTURE) == -1) return 2;

    pid_t pid = fork();
    if (pid == 0) {
      if (!prepare_environment()) _exit(2);

      static char error_info[402];
      size_t n = 0;
      if (shift) error_info[n++] = 'a';
      for (int i = 0; i < 200; i++) { error_info[n++] = (char)0xc3; error_info[n++] = (char)0xa9; }
      error_info[n] = '\0';
      status64.supported = true;
      status64.daemon_running = false;
      status64.daemon_error_info = error_info;

      update_status(NULL);

      _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 2;

    char got[4096];
    if (read_file(prop_path, got, sizeof(got)) == -1) return 2;
    if (strstr(got, "description=[") == NULL) return 2;

    if (!utf8_valid(got)) {
      printf("shift %d: module.prop is not valid UTF-8:\n%s\n", shift, got);

      return 1;
    }
  }

  return 0;
}

/* a description of two-byte characters shortened to make room for the
   lines after it: post_section stays valid UTF-8 (both offsets) */
static int test_description_utf8_whole(void) {
  for (int shift = 0; shift < 2; shift++) {
    char content[4096], desc[202], pad[901];
    size_t n = 0;
    if (shift) desc[n++] = 'a';
    for (int i = 0; i < 100; i++) { desc[n++] = (char)0xc3; desc[n++] = (char)0xa9; }
    desc[n] = '\0';
    const char *update = "updateJson=https://example.invalid/update.json\n";
    /* lines after the description: 900 bytes, room for 122 description bytes */
    size_t pad_len = 900 - strlen(update) - strlen("pad=") - 1;
    memset(pad, 'x', pad_len); pad[pad_len] = '\0';
    snprintf(content, sizeof(content), "id=rezygisk\ndescription=%s\n%spad=%s\n", desc, update, pad);
    if (write_prop(content) == -1) return 2;

    pid_t pid = fork();
    if (pid == 0) {
      if (!prepare_environment()) _exit(2);

      /* the description was shortened */
      if (strlen(post_section) >= strlen(desc) + 1 + 900) _exit(2);

      if (!utf8_valid(post_section)) {
        printf("shift %d: post_section is not valid UTF-8: %.130s\n", shift, post_section);

        _exit(1);
      }

      _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) return 2;
    if (WEXITSTATUS(status) != 0) return WEXITSTATUS(status);
  }

  return 0;
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

/* ---- monitor waits with a short REZYGISK_TRACE_TIMEOUT_MS ---- */
#ifndef REZYGISK_TRACE_TIMEOUT_MS
#define REZYGISK_TRACE_TIMEOUT_MS 10000
#endif

static double mon_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* app_process execs, the monitor sends SIGSTOP and the process never
   reports the stop: the monitor resumes it (SIGCONT), kills nothing,
   starts no tracer and does not detach it with a pending SIGSTOP */
static int test_stop_wait_times_out(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;

  pid_t child = fork();
  if (child == 0) {
    alarm(5);
    if (!prepare_environment()) _exit(2);
    if (!sigchld_listener_init()) _exit(2);

    tracing_state = TRACING;
    status64.supported = true;
    status64.daemon_running = true;
    status64.daemon_pid = 4242;
    test_program = "/system/bin/app_process64";

    sigchld_process = malloc(sizeof(pid_t));
    sigchld_process[0] = 5555;
    sigchld_process_count = 1;

    exited_pid = 5555;
    exited_status = ((SIGTRAP | (PTRACE_EVENT_EXEC << 8)) << 8) | 0x7f;
    hang_pid = 5555;
    echild_mode = true;
    kill(getpid(), SIGCHLD);

    double start = mon_now();
    sigchld_listener_callback();
    double took = mon_now() - start;

    if (took > 2.5 * REZYGISK_TRACE_TIMEOUT_MS / 1000.0) { printf("took %.2f s\n", took); _exit(1); }
    if (signals_sent[SIGKILL]) { printf("SIGKILL sent\n"); _exit(1); }
    if (!signals_sent[SIGSTOP] || !signals_sent[SIGCONT]) { printf("SIGSTOP %d, SIGCONT %d\n", signals_sent[SIGSTOP], signals_sent[SIGCONT]); _exit(1); }
    if (detach_with_sigstop) { printf("detached with SIGSTOP\n"); _exit(1); }

    _exit(0);
  }

  int status = 0;
  waitpid(child, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

  printf("child status 0x%x\n", status);

  return 1;
}

/* a traced process at its exec stop whose program cannot be read is
   detached (resumed) and forgotten, never left stopped */
static int test_exec_stop_unknown_program(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;

  pid_t child = fork();
  if (child == 0) {
    alarm(5);
    if (!prepare_environment()) _exit(2);
    if (!sigchld_listener_init()) _exit(2);

    tracing_state = TRACING;
    test_program = NULL;

    sigchld_process = malloc(sizeof(pid_t));
    sigchld_process[0] = 5555;
    sigchld_process_count = 1;

    exited_pid = 5555;
    exited_status = ((SIGTRAP | (PTRACE_EVENT_EXEC << 8)) << 8) | 0x7f;
    echild_mode = true;
    kill(getpid(), SIGCHLD);

    sigchld_listener_callback();

    if (plain_detaches != 1 || sigchld_process[0] != 0) {
      printf("%d detach(es), slot holds %d\n", plain_detaches, sigchld_process[0]);

      _exit(1);
    }

    _exit(0);
  }

  int status = 0;
  waitpid(child, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

  printf("child status 0x%x\n", status);

  return 1;
}

/* zygisk-ptrace ctl with the monitor's queue full: fails in time */
static int test_ctl_queue_full(void) {
  alarm(5);
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  mkdir(dir, 0700);
  if (chdir(dir) == -1) return 2;
  mkdir("rz", 0700);
  unlink("rz/" SOCKET_NAME);

  int rx = socket(AF_UNIX, SOCK_DGRAM, 0);
  struct sockaddr_un a = { .sun_family = AF_UNIX };
  snprintf(a.sun_path, sizeof(a.sun_path), "rz/" SOCKET_NAME);
  if (bind(rx, (struct sockaddr *)&a, sizeof(a)) == -1) return 2;

  /* fill the receiver's queue: new senders until a fresh one cannot send */
  char byte = 0;
  for (int n = 0; n < 64; n++) {
    int tx = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int sent = 0;
    while (sendto(tx, &byte, 1, 0, (struct sockaddr *)&a, sizeof(a)) == 1) sent++;
    if (errno != EAGAIN) return 2;
    if (sent == 0) break;
  }

  double start = mon_now();
  int ret = send_control_command(START);
  double took = mon_now() - start;

  if (ret == -1 && took < 2.5 * REZYGISK_TRACE_TIMEOUT_MS / 1000.0) return 0;

  printf("send_control_command %d after %.2f s\n", ret, took);

  return 1;
}

/* init is seized and forks at once: the SIGCHLD reporting its fork stop
   reaches the monitor's loop (waitpid is called), it is not discarded
   leaving init stopped */
static int test_sigchld_before_seize(void) {
  if (write_prop(PROP_FIXTURE) == -1) return 2;
  if (make_rz_dir() == -1) return 2;

  pid_t pid = fork();
  if (pid == -1) return 2;
  if (pid == 0) {
    alarm(3);
    sigchld_on_seize = true;
    exit_on_waitpid = true;
    init_monitor();

    _exit(1);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return 2;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

  printf("init fork stop never reached waitpid (child status 0x%x)\n", status);

  return 1;
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
  if (strcmp(argv[1], "epoll_fd_cloexec") == 0) return test_epoll_fd_cloexec();
  if (strcmp(argv[1], "status_text_utf8_whole") == 0) return test_status_text_utf8_whole();
  if (strcmp(argv[1], "description_utf8_whole") == 0) return test_description_utf8_whole();
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

  if (strcmp(argv[1], "stop_wait_times_out") == 0) return test_stop_wait_times_out();
  if (strcmp(argv[1], "exec_stop_unknown_program") == 0) return test_exec_stop_unknown_program();
  if (strcmp(argv[1], "ctl_queue_full") == 0) return test_ctl_queue_full();
  if (strcmp(argv[1], "sigchld_before_seize") == 0) return test_sigchld_before_seize();
  return 2;
}
