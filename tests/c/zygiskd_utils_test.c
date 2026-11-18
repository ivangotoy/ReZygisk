/* Host tests for zygiskd/src/utils.c. The source is included so static
   functions are reachable; libc calls that need a device are redirected
   with function-like macros defined before the include. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../zygiskd/src/utils.c"

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

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "read_string_empty") == 0) return test_read_string_empty();
  if (strcmp(argv[1], "read_string_value") == 0) return test_read_string_value();

  return 2;
}
