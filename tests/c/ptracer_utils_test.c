/* Host test for fork_dont_care() of loader/src/ptracer/utils.c. fork()
   can be made to fail on its second call; the ELF and maps helpers it
   links against are not used here and get empty stand-ins. */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define sigabbrev_np t_sigabbrev_np
#define sys_signame t_sys_signame
static const char *t_sys_signame[65];

static int fork_calls = 0;
static int fail_fork_call = 0;
static pid_t t_fork(void);
#define fork() t_fork()

/* waitpid modes for the tracer-wait tests:
   hang: the tracee never stops (WNOHANG reports nothing, a blocking call
         never returns); afterwards every ptrace request fails like on a
         running tracee
   syscall_stops: every wait reports a syscall stop at once */
enum { WAIT_REAL, WAIT_HANG, WAIT_SYSCALL_STOPS } wait_mode = WAIT_REAL;
static bool tracee_running = false;
static pid_t t_waitpid(pid_t pid, int *status, int options);
#define waitpid(p, s, o) t_waitpid(p, s, o)
static long t_ptrace(int request, ...);
#define ptrace(...) t_ptrace(__VA_ARGS__)
static ssize_t t_process_vm_readv(pid_t pid, const struct iovec *l, unsigned long ln, const struct iovec *r, unsigned long rn, unsigned long flags);
#define process_vm_readv(p, l, ln, r, rn, f) t_process_vm_readv(p, l, ln, r, rn, f)
static struct maps_info empty_maps;
static bool use_empty_maps = false;

#include "../../loader/src/ptracer/utils.c"

#undef fork

#undef waitpid
#undef ptrace
#undef process_vm_readv

static pid_t t_waitpid(pid_t pid, int *status, int options) {
  if (wait_mode == WAIT_REAL) return waitpid(pid, status, options);
  if (wait_mode == WAIT_SYSCALL_STOPS) {
    *status = ((SIGTRAP | 0x80) << 8) | 0x7f;

    return pid;
  }
  if (options & WNOHANG) {
    tracee_running = true;

    return 0;
  }

  for (;;) pause();
}

static long t_ptrace(int request, ...) {
  (void)request;
  if (tracee_running) {
    errno = ESRCH;

    return -1;
  }

  return 0;
}

/* the GOT slot never changes: the linker never resolves __libc_init */
static ssize_t t_process_vm_readv(pid_t pid, const struct iovec *l, unsigned long ln, const struct iovec *r, unsigned long rn, unsigned long flags) {
  (void)pid; (void)ln; (void)r; (void)rn; (void)flags;
  memset(l[0].iov_base, 0, l[0].iov_len);

  return (ssize_t)l[0].iov_len;
}

#ifndef REZYGISK_TRACE_TIMEOUT_MS
#define REZYGISK_TRACE_TIMEOUT_MS 10000
#endif
#ifndef REZYGISK_REMOTE_CALL_TIMEOUT_MS
#define REZYGISK_REMOTE_CALL_TIMEOUT_MS 25000
#endif

#include <time.h>
static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int in_time(double took, int limit_ms) {
  if (took < 2.5 * limit_ms / 1000.0) return 1;

  printf("took %.2f s (limit %d ms)\n", took, limit_ms);

  return 0;
}

/* a tracee that never reports its stop: the wait ends with the status of a
   failed wait (exited 255) */
static int test_trace_wait_times_out(void) {
  alarm(5);
  wait_mode = WAIT_HANG;

  int status = 0;
  double start = now_s();
  wait_for_trace(1234, &status, __WALL);
  if (!in_time(now_s() - start, REZYGISK_TRACE_TIMEOUT_MS)) return 1;
  if (status == (255 << 8)) return 0;

  printf("status 0x%x\n", status);

  return 1;
}

/* the wait on a remote syscall stop fails like a failed wait */
static int test_syscall_stop_times_out(void) {
  alarm(5);
  wait_mode = WAIT_HANG;

  int status = 0;
  double start = now_s();
  bool ok = wait_for_ptrace_syscall_stop(1234, &status);
  if (!in_time(now_s() - start, REZYGISK_TRACE_TIMEOUT_MS)) return 1;

  return ok ? 1 : 0;
}

/* a remote call whose function never returns: remote_call fails like a call
   whose registers cannot be read afterwards (returns 0) */
static int test_remote_call_times_out(void) {
  alarm(5);
  wait_mode = WAIT_HANG;

  struct user_regs_struct regs;
  memset(&regs, 0, sizeof(regs));
  regs.REG_SP = 0x7ff000;
  long args[1] = { 0 };

  double start = now_s();
  uintptr_t ret = remote_call(1234, &regs, 0x1000, 0x2000, args, 1);
  if (!in_time(now_s() - start, REZYGISK_REMOTE_CALL_TIMEOUT_MS)) return 1;

  return ret == 0 ? 0 : 1;
}

/* the linker never resolves __libc_init although the tracee keeps stopping
   at syscalls: wait_linker_ready gives up after the time limit */
static int test_linker_never_ready(void) {
  alarm(5);
  wait_mode = WAIT_SYSCALL_STOPS;
  use_empty_maps = true;

  uintptr_t resolved = 0, slot = 0x2000;
  double start = now_s();
  bool ok = wait_linker_ready(1234, &resolved, &slot);
  if (!in_time(now_s() - start, REZYGISK_TRACE_TIMEOUT_MS)) return 1;

  return ok ? 1 : 0;
}

/* the intermediate child of fork_dont_care is never reported: the caller
   still gets a pid, as when waitpid fails with ECHILD */
static int test_fork_reap_times_out(void) {
  alarm(5);
  pid_t me = getpid();
  wait_mode = WAIT_HANG;

  double start = now_s();
  int ret = fork_dont_care();
  if (ret == 0 || getpid() != me) _exit(0);
  if (!in_time(now_s() - start, REZYGISK_TRACE_TIMEOUT_MS)) return 1;

  return ret > 0 ? 0 : 1;
}


static pid_t t_fork(void) {
  fork_calls++;
  if (fork_calls == fail_fork_call) {
    errno = EAGAIN;

    return -1;
  }

  return fork();
}

ElfImg *ElfImg_create(const char *elf, void *base) { (void)elf; (void)base; return NULL; }
void ElfImg_destroy(ElfImg *img) { (void)img; }
ElfW(Addr) getSymbAddress(ElfImg *img, const char *name) { (void)img; (void)name; return 0; }
struct maps_info *parse_maps(const char *pid) { (void)pid; return use_empty_maps ? &empty_maps : NULL; }
void free_maps(struct maps_info *maps) { (void)maps; }

/* the second fork (in the intermediate child) fails: no process other
   than the caller may return from fork_dont_care(), and the caller must
   see the failure */
static int test_second_fork_fails(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  mkdir(dir, 0700);

  char marker[PATH_MAX];
  snprintf(marker, sizeof(marker), "%s/returned", dir);
  unlink(marker);

  pid_t me = getpid();
  fail_fork_call = 2;

  int ret = fork_dont_care();
  if (getpid() != me) {
    /* a child came back to the caller's code: in the monitor it would
       carry on as a second monitor */
    FILE *f = fopen(marker, "w");
    if (f) fclose(f);

    _exit(0);
  }

  if (access(marker, F_OK) == 0) {
    printf("the intermediate child returned from fork_dont_care()\n");

    return 1;
  }
  if (ret != -1) {
    printf("caller got %d, expected -1\n", ret);

    return 1;
  }

  return 0;
}

/* normal case: the grandchild gets 0, the caller a positive pid */
static int test_fork_ok(void) {
  pid_t me = getpid();

  int fds[2];
  if (pipe(fds) == -1) return 2;

  int ret = fork_dont_care();
  if (ret == 0) {
    write(fds[1], "g", 1);

    _exit(0);
  }
  if (getpid() != me) _exit(0);

  char c = 0;
  close(fds[1]);
  read(fds[0], &c, 1);

  return ret > 0 && c == 'g' ? 0 : 1;
}

/* SIGCHLD ignored (a disposition inherited across exec): the kernel reaps
   the intermediate child itself and waitpid() fails with ECHILD after a
   successful double fork; the caller must still get a pid, not -1 */
static int test_fork_ok_sigchld_ignored(void) {
  signal(SIGCHLD, SIG_IGN);

  return test_fork_ok();
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "fork_ok") == 0) return test_fork_ok();
  if (strcmp(argv[1], "fork_ok_sigchld_ignored") == 0) return test_fork_ok_sigchld_ignored();
  if (strcmp(argv[1], "second_fork_fails") == 0) return test_second_fork_fails();

  if (strcmp(argv[1], "trace_wait_times_out") == 0) return test_trace_wait_times_out();
  if (strcmp(argv[1], "syscall_stop_times_out") == 0) return test_syscall_stop_times_out();
  if (strcmp(argv[1], "remote_call_times_out") == 0) return test_remote_call_times_out();
  if (strcmp(argv[1], "linker_never_ready") == 0) return test_linker_never_ready();
  if (strcmp(argv[1], "fork_reap_times_out") == 0) return test_fork_reap_times_out();

  return 2;
}
