/* Host test for trace_zygote() in loader/src/ptracer/ptracer.c. ptrace
   and kill never touch a real process; the stops a traced zygote would
   report come from a script, and the remote injection helpers it calls
   are stand-ins that succeed. */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define sigabbrev_np t_sigabbrev_np
#define sys_signame t_sys_signame
static const char *t_sys_signame[65];

static int detached = 0;
static int sigkills = 0;
static long t_ptrace(int request, ...);
#define ptrace(...) t_ptrace(__VA_ARGS__)
#define kill(pid, sig) ((void)(pid), ((sig) == SIGKILL ? sigkills++ : 0), 0)

#include "../../loader/src/ptracer/ptracer.c"

#undef ptrace
#undef kill

/* a timed-out wait reports exited 255 while the tracee keeps running; ptrace
   requests on a running tracee fail */
static int last_status = 0;
#define TIMED_OUT (255 << 8)

static long t_ptrace(int request, ...) {
  if (last_status == TIMED_OUT) {
    errno = ESRCH;

    return -1;
  }
  if (request == PTRACE_DETACH) detached++;

  return 0;
}

/* stop statuses as waitpid() reports them */
#define EVENT_STOP(sig) ((((sig) | (PTRACE_EVENT_STOP << 8)) << 8) | 0x7f)
#define PLAIN_STOP(sig) (((sig) << 8) | 0x7f)

static int script[8];
static int script_len = 0, script_pos = 0;

void wait_for_trace(int pid, int *status, int flags) {
  (void)pid; (void)flags;
  *status = script_pos < script_len ? script[script_pos++] : 0;
  last_status = *status;
}

static int later_waits = 0;
bool wait_for_ptrace_syscall_stop(int pid, int *status) { (void)pid; *status = 0; later_waits++; return true; }
bool wait_linker_ready(int pid, uintptr_t *resolved, uintptr_t *slot) { (void)pid; *resolved = 0x1000; *slot = 0x2000; later_waits++; return true; }
bool ptrace_poke_uintptr(int pid, uintptr_t addr, uintptr_t value) { (void)pid; (void)addr; (void)value; return true; }
bool get_regs(int pid, struct user_regs_struct *regs) { (void)pid; memset(regs, 0, sizeof(*regs)); return true; }
bool set_regs(int pid, struct user_regs_struct *regs) { (void)pid; (void)regs; return true; }
static struct maps_info empty_maps;
struct maps_info *parse_maps(const char *pid) { (void)pid; return &empty_maps; }
void free_maps(struct maps_info *maps) { (void)maps; }
void *find_module_return_addr(struct maps_info *map, const char *suffix) { (void)map; (void)suffix; return (void *)0x3000; }
bool remote_csoloader_load_and_resolve_entry(int pid, struct user_regs_struct *regs, struct maps_info *map, struct maps_info *local_map,
                                             const char *lib_path, uintptr_t *base, size_t *size, uintptr_t *entry) {
  (void)pid; (void)regs; (void)map; (void)local_map; (void)lib_path;
  *base = 0x4000; *size = 0x1000; *entry = 0x5000;

  return true;
}
uintptr_t remote_call(int pid, struct user_regs_struct *regs, uintptr_t func_addr, uintptr_t return_addr, long *args, size_t args_size) {
  (void)pid; (void)func_addr; (void)args; (void)args_size;
  regs->REG_IP = (long)return_addr;

  return 0;
}
struct kernel_version parse_kversion() { struct kernel_version v = { .major = 5, .minor = 10, .patch = 0 }; return v; }
void parse_status(int status, char *buf, size_t len) { snprintf(buf, len, "0x%x", status); }

/* seize stop, GOT break (SIGSEGV), group-stop after SIGCONT, then <last> */
static int run(int last, bool want_ok) {
  int s[] = { EVENT_STOP(SIGSTOP), PLAIN_STOP(SIGSEGV), EVENT_STOP(SIGTRAP), last, 0 };
  memcpy(script, s, sizeof(s));
  script_len = 5;

  bool ok = trace_zygote(1234, false);
  if (ok == want_ok && detached == 1) return 0;

  printf("trace_zygote returned %d with %d detach(es), expected %d with 1\n", ok, detached, want_ok);

  return 1;
}

/* a wait that times out at <pos> of the normal sequence: trace_zygote fails
   (the caller then kills zygote, as for any failed injection) without
   claiming a detach it could not do, and kills nothing itself */
static int timed_out_at(int pos, int want_later_waits) {
  int s[] = { EVENT_STOP(SIGSTOP), PLAIN_STOP(SIGSEGV), EVENT_STOP(SIGTRAP), PLAIN_STOP(SIGCONT), PLAIN_STOP(SIGTRAP) };
  s[pos] = TIMED_OUT;
  memcpy(script, s, sizeof(s));
  script_len = 5;

  bool ok = trace_zygote(1234, false);
  if (!ok && sigkills == 0 && (want_later_waits < 0 || later_waits == want_later_waits)) return 0;

  printf("trace_zygote returned %d, %d SIGKILL(s), %d later wait(s); expected 0, 0, %d\n", ok, sigkills, later_waits, want_later_waits);

  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "sigcont_detaches") == 0) return run(PLAIN_STOP(SIGCONT), true);
  if (strcmp(argv[1], "unexpected_stop_detaches") == 0) return run(PLAIN_STOP(SIGSEGV), true);
  if (strcmp(argv[1], "seize_wait_times_out") == 0) return timed_out_at(0, 0);
  if (strcmp(argv[1], "group_stop_wait_times_out") == 0) return timed_out_at(2, -1);
  if (strcmp(argv[1], "sigcont_wait_times_out") == 0) return timed_out_at(3, -1);
  if (strcmp(argv[1], "final_wait_times_out") == 0) return timed_out_at(4, -1);

  return 2;
}
