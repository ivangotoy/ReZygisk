/* Host test for read_string() of loader/src/common/socket_utils.c.
   malloc() is wrapped to see the size it is asked for. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void *t_malloc(size_t size);
#define malloc(n) t_malloc(n)

#include "../../loader/src/common/socket_utils.c"

#undef malloc

static int wrapped_allocations = 0;

/* a zero-byte request here can only come from str_len + 1 wrapping */
static void *t_malloc(size_t size) {
  if (size == 0) {
    wrapped_allocations++;

    return NULL;
  }

  return malloc(size);
}

static int test_read_string_huge_length(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) return 2;

  size_t len = SIZE_MAX;
  if (write(sv[0], &len, sizeof(len)) != (ssize_t)sizeof(len)) return 2;
  if (write(sv[0], "abcd", 4) != 4) return 2;
  close(sv[0]);

  char *s = read_string(sv[1]);
  if (s != NULL || wrapped_allocations != 0) {
    printf("read_string returned %p after %d wrapped allocation(s)\n", (void *)s, wrapped_allocations);

    return 1;
  }

  return 0;
}

static int test_read_string_value(void) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) return 2;

  if (write_string(sv[0], "/data/adb/modules/example/zygisk/arm64-v8a.so") == -1) return 2;

  char *s = read_string(sv[1]);
  int ok = s != NULL && strcmp(s, "/data/adb/modules/example/zygisk/arm64-v8a.so") == 0;
  free(s);

  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "read_string_value") == 0) return test_read_string_value();
  if (strcmp(argv[1], "read_string_huge_length") == 0) return test_read_string_huge_length();

  return 2;
}
