/* Host test for loader/src/common/daemon.c with ReZygiskd not running
   (its socket path does not exist on the host). Linked with the real
   loader/src/common/socket_utils.c. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../loader/src/common/daemon.c"

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

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "get_info_daemon_down") == 0) return test_get_info_daemon_down();

  return 2;
}
