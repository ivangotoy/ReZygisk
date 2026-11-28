/* Host tests for the argument handling of loader/src/ptracer/main.c.
   Tracing, killing and daemon calls are stand-ins that only count. */
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ZKSU_VERSION
#define ZKSU_VERSION "test"
#endif

static int traced = 0, killed = 0, restarted = 0, sent = 0;

#define main rz_main
#define kill(pid, sig) ((void)(pid), (void)(sig), killed++, 0)

#include "../../loader/src/ptracer/main.c"

#undef main

void init_monitor() {}
bool trace_zygote(int pid, bool is_tango) { (void)pid; (void)is_tango; traced++; return true; }
int send_control_command(enum rezygiskd_command cmd) { (void)cmd; sent++; return 0; }
void rezygiskd_zygote_restart() { restarted++; }
void rezygiskd_get_info(struct rezygisk_info *info) { memset(info, 0, sizeof(*info)); }
void free_rezygisk_info(struct rezygisk_info *info) { (void)info; }

/* runs rz_main in a child; returns its exit code, or 128 + signal */
static int run(int argc, char **argv) {
  fflush(stdout);

  pid_t pid = fork();
  if (pid == 0) {
    int ret = rz_main(argc, argv);
    printf("\ncounts traced=%d killed=%d restarted=%d sent=%d\n", traced, killed, restarted, sent);

    _exit(ret);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);

  return WEXITSTATUS(status);
}

/* a rejected pid must not restart, trace or kill anything (kill(0) would
   hit the caller's process group) */
static int bad_pid(const char *pid_arg) {
  char *argv[] = { "zygisk-ptrace", "trace", (char *)pid_arg, "--restart", NULL };
  fflush(stdout);

  int fds[2];
  if (pipe(fds) == -1) return 2;

  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int ret = rz_main(4, argv);
    int counts[3] = { traced, killed, restarted };
    write(fds[1], counts, sizeof(counts));

    _exit(ret);
  }
  close(fds[1]);

  int counts[3] = { -1, -1, -1 };
  read(fds[0], counts, sizeof(counts));
  int status = 0;
  waitpid(pid, &status, 0);

  if (WIFEXITED(status) && WEXITSTATUS(status) == 1 && counts[0] == 0 && counts[1] == 0 && counts[2] == 0) return 0;

  printf("pid \"%s\": exit %d traced=%d killed=%d restarted=%d\n", pid_arg, WIFEXITED(status) ? WEXITSTATUS(status) : -1, counts[0], counts[1], counts[2]);

  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "trace_valid_pid") == 0) {
    char *a[] = { "zygisk-ptrace", "trace", "1234", NULL };

    /* exit 0 and the pid actually traced, once */
    pid_t pid = fork();
    if (pid == -1) return 2;
    if (pid == 0) _exit(rz_main(3, a) != 0 ? 1 : traced == 1 ? 0 : 3);

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return 2;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

    printf("trace 1234: status 0x%x (0x100: failed, 0x300: not traced once)\n", status);

    return 1;
  }
  if (strcmp(argv[1], "trace_non_numeric_pid") == 0) return bad_pid("abc");
  if (strcmp(argv[1], "trace_trailing_garbage_pid") == 0) return bad_pid("12abc");
  if (strcmp(argv[1], "trace_zero_pid") == 0) return bad_pid("0");
  if (strcmp(argv[1], "trace_negative_pid") == 0) return bad_pid("-5");
  if (strcmp(argv[1], "ctl_without_command") == 0) {
    char *a[] = { "zygisk-ptrace", "ctl", NULL };
    int ret = run(2, a);
    if (ret == 1) return 0;

    printf("ctl without a command: exit %d\n", ret);

    return 1;
  }

  return 2;
}
