/* Host test for fork_dont_care() of loader/src/ptracer/utils.c. fork()
   can be made to fail on its second call; the ELF and maps helpers it
   links against are not used here and get empty stand-ins. */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

#include "../../loader/src/ptracer/utils.c"

#undef fork

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
struct maps_info *parse_maps(const char *pid) { (void)pid; return NULL; }
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

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "fork_ok") == 0) return test_fork_ok();
  if (strcmp(argv[1], "second_fork_fails") == 0) return test_second_fork_fails();

  return 2;
}
