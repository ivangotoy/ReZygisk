/* Host tests for zygiskd/src/utils.c. The source is included so static
   functions are reachable; libc calls that need a device are redirected
   with function-like macros defined before the include. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* fopen/fclose: paths of the device are mapped to files the test writes,
   and open FILEs are counted to catch leaks */
static FILE *t_fopen(const char *path, const char *mode);
static int t_fclose(FILE *f);
#define fopen(p, m) t_fopen(p, m)
#define fclose(f) t_fclose(f)

#include "../../zygiskd/src/utils.c"

static char attr_current_path[PATH_MAX];
static int open_files = 0;

static FILE *t_fopen(const char *path, const char *mode) {
  if (strcmp(path, "/proc/self/attr/current") == 0) path = attr_current_path;

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

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "read_string_empty") == 0) return test_read_string_empty();
  if (strcmp(argv[1], "read_string_value") == 0) return test_read_string_value();
  if (strcmp(argv[1], "current_attr_bounds") == 0) return test_current_attr_bounds();

  return 2;
}
