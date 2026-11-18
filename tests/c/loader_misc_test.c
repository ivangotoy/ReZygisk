/* Host test for parse_maps() of loader/src/common/misc.c. The maps file
   is redirected to a fixture and heap blocks are counted. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static FILE *t_fopen(const char *path, const char *mode);
static void *t_malloc(size_t size);
static void *t_calloc(size_t n, size_t size);
static void *t_realloc(void *p, size_t size);
static char *t_strdup(const char *s);
static void t_free(void *p);
#define fopen(p, m) t_fopen(p, m)
#define malloc(n) t_malloc(n)
#define calloc(n, s) t_calloc(n, s)
#define realloc(p, n) t_realloc(p, n)
#define strdup(s) t_strdup(s)
#define free(p) t_free(p)

#include "../../loader/src/common/misc.c"

#undef fopen
#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef free

static int live_blocks = 0;
static char maps_path[PATH_MAX];

static FILE *t_fopen(const char *path, const char *mode) {
  if (strncmp(path, "/proc/", 6) == 0 && strstr(path, "/maps")) path = maps_path;

  return fopen(path, mode);
}

static void *t_malloc(size_t size) { void *p = malloc(size); if (p) live_blocks++; return p; }
static void *t_calloc(size_t n, size_t size) { void *p = calloc(n, size); if (p) live_blocks++; return p; }
static void *t_realloc(void *old, size_t size) { void *p = realloc(old, size); if (p && !old) live_blocks++; return p; }
static char *t_strdup(const char *s) { char *p = strdup(s); if (p) live_blocks++; return p; }
static void t_free(void *p) { if (p) live_blocks--; free(p); }

static int write_maps(const char *content) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return -1;
  mkdir(dir, 0700);
  snprintf(maps_path, sizeof(maps_path), "%s/maps", dir);

  FILE *f = fopen(maps_path, "w");
  if (f == NULL) return -1;
  fputs(content, f);
  fclose(f);

  return 0;
}

/* no mapping found: NULL and nothing left allocated */
static int test_empty_maps_no_leak(void) {
  if (write_maps("") == -1) return 2;

  struct maps_info *info = parse_maps("1234");
  if (info == NULL && live_blocks == 0) return 0;

  printf("parse_maps returned %p with %d block(s) still allocated\n", (void *)info, live_blocks);

  return 1;
}

/* normal file: entries parsed, everything freed by free_maps */
static int test_maps_parsed(void) {
  if (write_maps("7f0000000000-7f0000001000 r-xp 00000000 fd:01 1234 /system/lib64/libexample.so\n"
                 "7f0000001000-7f0000002000 rw-p 00001000 fd:01 1234 /system/lib64/libexample.so\n"
                 "7f0000002000-7f0000003000 rw-p 00000000 00:00 0\n") == -1) return 2;

  struct maps_info *info = parse_maps("1234");
  if (info == NULL || info->length != 3 || strcmp(info->maps[0].path, "/system/lib64/libexample.so") != 0) return 1;

  free_maps(info);

  return live_blocks == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "maps_parsed") == 0) return test_maps_parsed();
  if (strcmp(argv[1], "empty_maps_no_leak") == 0) return test_empty_maps_no_leak();

  return 2;
}
