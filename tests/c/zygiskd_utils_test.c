/* Host tests for zygiskd/src/utils.c. The source is included so static
   functions are reachable; libc calls that need a device are redirected
   with function-like macros defined before the include. */
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>

/* fopen/fclose: paths of the device are mapped to files the test writes,
   and open FILEs are counted to catch leaks */
static FILE *t_fopen(const char *path, const char *mode);
static int t_fclose(FILE *f);
#define fopen(p, m) t_fopen(p, m)
#define fclose(f) t_fclose(f)

/* namespace calls: setns succeeds, unshare fails, umount2 is recorded */
static int t_umount2(const char *target, int flags);
#define setns(fd, type) ((void)(fd), (void)(type), 0)
/* unshare fails unless a test clears unshare_fails */
static bool unshare_fails = true;
static bool hang_in_umount = false;
#define unshare(flags) ((void)(flags), unshare_fails ? (errno = EPERM, -1) : 0)
#define umount2(t, f) t_umount2(t, f)

#include "../../zygiskd/src/utils.c"

static char attr_current_path[PATH_MAX];
static char mountinfo_path[PATH_MAX];
static int open_files = 0;

static FILE *t_fopen(const char *path, const char *mode) {
  if (strcmp(path, "/proc/self/attr/current") == 0) path = attr_current_path;
  if (strcmp(path, "/proc/self/mountinfo") == 0) path = mountinfo_path;
  /* sockcreate: the thread-self file is missing, the per-tid one rejects writes */
  if (strcmp(path, "/proc/thread-self/attr/sockcreate") == 0) {
    errno = ENOENT;

    return NULL;
  }
  if (strncmp(path, "/proc/self/task/", strlen("/proc/self/task/")) == 0) {
    path = attr_current_path;
    mode = "r";
  }

  FILE *f = (fopen)(path, mode);
  if (f) open_files++;

  return f;
}

static int t_fclose(FILE *f) {
  open_files--;

  return (fclose)(f);
}

static int make_test_dir(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return -1;
  if (mkdir(dir, 0700) == -1 && errno != EEXIST) return -1;

  return 0;
}

/* writes <len> bytes of <c> to $TEST_DIR/<name> and returns its path in out */
static int write_fixture(const char *name, char c, size_t len, char *out) {
  if (make_test_dir() == -1) return -1;
  snprintf(out, PATH_MAX, "%s/%s", getenv("TEST_DIR"), name);

  FILE *f = (fopen)(out, "w");
  if (f == NULL) return -1;
  for (size_t i = 0; i < len; i++) fputc(c, f);
  (fclose)(f);

  return 0;
}

int __system_property_get(const char *name, char *value) {
  (void)name;
  value[0] = '\0';

  return 0;
}

/* read_string() with an empty string must leave an empty C string */
static int test_read_string_empty(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) return 2;

  size_t len = 0;
  if (write(sv[0], &len, sizeof(len)) != (ssize_t)sizeof(len)) return 2;

  char buf[16];
  memset(buf, 'X', sizeof(buf));

  ssize_t ret = read_string(sv[1], buf, sizeof(buf));
  if (ret != 0) return 1;

  return buf[0] == '\0' ? 0 : 1;
}

/* read_string() with a normal string */
static int test_read_string_value(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) return 2;

  const char *s = "com.example.app";
  if (write_string(sv[0], s) == -1) return 2;

  char buf[32];
  memset(buf, 'X', sizeof(buf));

  if (read_string(sv[1], buf, sizeof(buf)) != (ssize_t)strlen(s)) return 1;

  return strcmp(buf, s) == 0 ? 0 : 1;
}

/* get_current_attr() must stay inside the buffer when the attribute fills it */
static int test_current_attr_bounds(void) {
  if (write_fixture("attr_current", 'a', 16, attr_current_path) == -1) return 2;

  struct {
    char buf[16];
    char canary;
  } s;
  s.canary = 'C';

  if (!get_current_attr(s.buf, sizeof(s.buf))) return 1;
  if (s.canary != 'C') {
    printf("byte after the buffer overwritten\n");

    return 1;
  }

  return strlen(s.buf) == sizeof(s.buf) - 1 ? 0 : 1;
}

/* set_socket_create_context() must close the tid file when the write fails */
static int test_sockcreate_no_leak(void) {
  if (write_fixture("sockcreate", 'x', 1, attr_current_path) == -1) return 2;

  set_socket_create_context("u:r:zygote:s0");
  if (open_files != 0) {
    printf("%d FILE(s) left open\n", open_files);

    return 1;
  }

  return 0;
}

static int t_umount2(const char *target, int flags) {
  (void)flags;

  if (hang_in_umount) {
    char pidfile[PATH_MAX];
    snprintf(pidfile, sizeof(pidfile), "%s/helper.pid", getenv("TEST_DIR"));
    FILE *p = (fopen)(pidfile, "w");
    if (p) { fprintf(p, "%d\n", (int)getpid()); (fclose)(p); }
    sleep(30);
  }

  char log[PATH_MAX];
  snprintf(log, sizeof(log), "%s/umounts", getenv("TEST_DIR"));

  FILE *f = (fopen)(log, "a");
  if (f) {
    fprintf(f, "%s\n", target);
    (fclose)(f);
  }

  return 0;
}

/* save_mns_fd(Clean) must not unmount anything when unshare() fails: the
   child would still be in the target's mount namespace */
static int test_clean_ns_unshare_fails(void) {
  if (make_test_dir() == -1) return 2;

  snprintf(mountinfo_path, sizeof(mountinfo_path), "%s/mountinfo", getenv("TEST_DIR"));
  FILE *f = (fopen)(mountinfo_path, "w");
  if (f == NULL) return 2;
  fprintf(f, "20 1 0:18 / / rw,relatime shared:1 - ext4 /dev/root rw\n");
  fprintf(f, "36 20 0:20 / /data/adb/modules rw,relatime shared:2 - tmpfs magisk rw\n");
  (fclose)(f);

  char log[PATH_MAX];
  snprintf(log, sizeof(log), "%s/umounts", getenv("TEST_DIR"));
  unlink(log);

  struct root_impl impl = { .impl = Magisk, .variant = 0 };
  int fd = save_mns_fd(getpid(), Clean, impl);

  if (access(log, F_OK) == 0) {
    printf("umount2 ran in the target namespace\n");

    return 1;
  }

  return fd == -1 ? 0 : 1;
}

/* a failed save_mns_fd() must reap its helper child (no zombie left) */
static int test_clean_ns_failure_reaps_child(void) {
  int ret = test_clean_ns_unshare_fails();
  if (ret != 0) return ret;

  pid_t left = waitpid(-1, NULL, 0);
  if (left == -1 && errno == ECHILD) return 0;

  printf("child %d was left for the caller to reap\n", (int)left);

  return 1;
}

/* a successful save_mns_fd(Mounted) logs no error (LOGx used without
   braces under an if must stay a single statement) */
/* a state that is neither Clean nor Mounted is refused before any work:
   such an fd would be cached nowhere and never closed */
static int test_mns_unknown_state_refused(void) {
  struct root_impl impl = { .impl = Magisk, .variant = 0 };
  errno = 0;
  int ns_fd = save_mns_fd(getpid(), (enum MountNamespaceState)2, impl);
  if (ns_fd == -1 && errno == EINVAL) return 0;

  printf("save_mns_fd(state 2) returned %d, errno %d\n", ns_fd, errno);

  return 1;
}

static int test_mounted_ns_logs_no_error(void) {
  if (make_test_dir() == -1) return 2;

  char out[PATH_MAX];
  snprintf(out, sizeof(out), "%s/stdout", getenv("TEST_DIR"));
  int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd == -1) return 2;

  int saved = dup(STDOUT_FILENO);
  dup2(fd, STDOUT_FILENO);
  close(fd);

  struct root_impl impl = { .impl = Magisk, .variant = 0 };
  int ns_fd = save_mns_fd(getpid(), Mounted, impl);

  dup2(saved, STDOUT_FILENO);
  close(saved);

  if (ns_fd == -1) return 2;

  char buf[1024] = { 0 };
  FILE *f = (fopen)(out, "r");
  if (f == NULL) return 2;
  fread(buf, 1, sizeof(buf) - 1, f);
  (fclose)(f);

  if (strstr(buf, "Failed") == NULL) return 0;

  printf("logged on success: %s\n", buf);

  return 1;
}

/* ---- time limits (built with a short ZYGISKD_PEER_TIMEOUT_MS) ---- */
#include <time.h>

#ifndef ZYGISKD_PEER_TIMEOUT_MS
#define ZYGISKD_PEER_TIMEOUT_MS 10000
#endif

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* pid a test child wrote to $TEST_DIR/<name>; 0 when missing */
static pid_t recorded_pid(const char *name) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", getenv("TEST_DIR"), name);
  FILE *f = (fopen)(path, "r");
  if (f == NULL) return 0;
  int pid = 0;
  if (fscanf(f, "%d", &pid) != 1) pid = 0;
  (fclose)(f);

  return pid;
}

/* the child is gone: not running and not left as a zombie */
static int child_gone(pid_t pid) {
  if (pid <= 0) return 0;
  if (kill(pid, 0) == -1 && errno == ESRCH) return 1;

  printf("child %d still exists\n", (int)pid);

  return 0;
}

static int exec_timeout(const char *script, bool want_ok, const char *want_out) {
  alarm(5);
  if (make_test_dir() == -1) return 2;
  if (chdir(getenv("TEST_DIR")) == -1) return 2;
  unlink("child.pid");

  const char *const argv[] = { "sh", "-c", script, NULL };
  char buf[32];
  memset(buf, 'X', sizeof(buf));

  double start = now_s();
  bool ok = exec_command(buf, sizeof(buf), "/bin/sh", argv);
  double took = now_s() - start;

  if (ok != want_ok) { printf("exec_command returned %d after %.2f s\n", ok, took); return 1; }
  if (want_out && strcmp(buf, want_out) != 0) { printf("output \"%s\", expected \"%s\"\n", buf, want_out); return 1; }
  if (!want_ok && took > 2.5 * ZYGISKD_PEER_TIMEOUT_MS / 1000.0) { printf("took %.2f s\n", took); return 1; }
  if (!want_ok && !child_gone(recorded_pid("child.pid"))) return 1;

  return 0;
}

/* namespace helper stuck in umount: save_mns_fd fails in time, helper gone */
static int test_clean_ns_helper_hangs(void) {
  alarm(5);
  if (make_test_dir() == -1) return 2;

  snprintf(mountinfo_path, sizeof(mountinfo_path), "%s/mountinfo", getenv("TEST_DIR"));
  FILE *f = (fopen)(mountinfo_path, "w");
  if (f == NULL) return 2;
  fprintf(f, "36 20 0:20 / /data/adb/modules rw,relatime shared:2 - tmpfs magisk rw\n");
  (fclose)(f);

  unshare_fails = false;
  hang_in_umount = true;

  double start = now_s();
  int fd = save_mns_fd(getpid(), Clean, (struct root_impl){ .impl = Magisk, .variant = 0 });
  double took = now_s() - start;

  if (fd != -1 || took > 2.5 * ZYGISKD_PEER_TIMEOUT_MS / 1000.0) { printf("save_mns_fd %d after %.2f s\n", fd, took); return 1; }

  return child_gone(recorded_pid("helper.pid")) ? 0 : 1;
}

/* the monitor's datagram queue is full: unix_datagram_sendto gives up in
   time through its error path */
static int test_datagram_queue_full(void) {
  alarm(5);
  if (write_fixture("attr_current", 'a', 8, attr_current_path) == -1) return 2;
  if (chdir(getenv("TEST_DIR")) == -1) return 2;
  unlink("mon.sock");

  int rx = socket(AF_UNIX, SOCK_DGRAM, 0);
  struct sockaddr_un a = { .sun_family = AF_UNIX };
  snprintf(a.sun_path, sizeof(a.sun_path), "mon.sock");
  if (bind(rx, (struct sockaddr *)&a, sizeof(a)) == -1) return 2;

  /* new senders until a fresh one cannot queue anything */
  char byte = 0;
  for (int n = 0; n < 64; n++) {
    int tx = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int sent = 0;
    while (sendto(tx, &byte, 1, 0, (struct sockaddr *)&a, sizeof(a)) == 1) sent++;
    if (errno != EAGAIN) return 2;
    if (sent == 0) break;
  }

  double start = now_s();
  unix_datagram_sendto("mon.sock", "x", 1);
  double took = now_s() - start;

  if (took < 2.5 * ZYGISKD_PEER_TIMEOUT_MS / 1000.0) return 0;

  printf("took %.2f s\n", took);

  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "read_string_empty") == 0) return test_read_string_empty();
  if (strcmp(argv[1], "read_string_value") == 0) return test_read_string_value();
  if (strcmp(argv[1], "current_attr_bounds") == 0) return test_current_attr_bounds();
  if (strcmp(argv[1], "sockcreate_no_leak") == 0) return test_sockcreate_no_leak();
  if (strcmp(argv[1], "clean_ns_unshare_fails") == 0) return test_clean_ns_unshare_fails();
  if (strcmp(argv[1], "clean_ns_helper_hangs") == 0) return test_clean_ns_helper_hangs();
  if (strcmp(argv[1], "datagram_queue_full") == 0) return test_datagram_queue_full();
  if (strcmp(argv[1], "exec_output") == 0) return exec_timeout("echo hello", true, "hello");
  if (strcmp(argv[1], "exec_split_output") == 0) return exec_timeout("printf 12; sleep 0.1; printf '345\\n'", true, "12345");
  if (strcmp(argv[1], "exec_child_hangs") == 0) return exec_timeout("echo $$ > child.pid; exec sleep 30", false, NULL);
  if (strcmp(argv[1], "exec_child_hangs_after_closing") == 0) return exec_timeout("echo $$ > child.pid; exec >&-; exec sleep 30", false, NULL);
  if (strcmp(argv[1], "clean_ns_failure_reaps_child") == 0) return test_clean_ns_failure_reaps_child();
  if (strcmp(argv[1], "mns_unknown_state_refused") == 0) return test_mns_unknown_state_refused();
  if (strcmp(argv[1], "mounted_ns_logs_no_error") == 0) return test_mounted_ns_logs_no_error();

  return 2;
}
