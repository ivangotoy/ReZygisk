/* Host tests for how zygiskd/src/root_impl/apatch.c and magisk.c parse
   the output of the root manager binaries. exec_command() returns canned
   output, access() and uid_from_pkg() are stand-ins. */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* values of common.mk */
#define MIN_APATCH_VERSION 10655
#define MIN_MAGISK_VERSION 26402

#define exec_command t_exec
#define uid_from_pkg t_uid_from_pkg
#define access(p, m) ((void)(p), (void)(m), 0)
/* APatch's package_config is read from $TEST_DIR/package_config */
static FILE *t_fopen(const char *path, const char *mode);
#define fopen(p, m) t_fopen(p, m)

#include "../../zygiskd/src/root_impl/apatch.c"
#include "../../zygiskd/src/root_impl/magisk.c"

#undef fopen

static FILE *t_fopen(const char *path, const char *mode) {
  if (strcmp(path, "/data/adb/ap/package_config") != 0) return fopen(path, mode);

  char mapped[PATH_MAX];
  snprintf(mapped, sizeof(mapped), "%s/package_config", getenv("TEST_DIR"));

  return fopen(mapped, mode);
}

/* isolated service whose uid matches no entry: the entry whose package
   name the process name starts with, up to its end or a ":" suffix */
static int apatch_isolated_prefix(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  mkdir(dir, 0700);
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/package_config", dir);
  FILE *f = fopen(path, "w");
  if (f == NULL) return 2;
  fputs("pkg,exclude,allow,uid,to_uid,sctx\ncom.example.app,1,0,10500,0,x\ncom.example.other,0,0,10600,0,x\n", f);
  fclose(f);

  struct { const char *process; bool want; } cases[] = {
    { "com.example.app:isolated", true },
    { "com.example.app", true },
    { "com.example.app_zygote", true },
    { "com.example.app_other", false },
    { "com.example.app2:isolated", false },
    { "com.example.ap", false },
    { "", false },
  };
  int bad = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    bool got = apatch_uid_should_umount(99001, cases[i].process);
    if (got != cases[i].want) { printf("\"%s\": umount %d, expected %d\n", cases[i].process, got, cases[i].want); bad = 1; }
  }

  return bad;
}

/* canned output: <text>, a NUL, then <after> at byte offset <after_at>
   (bytes past the string model whatever the stack buffer held) */
static const char *out_text = "";
static const char *out_after = "";
static size_t out_after_at = 0;
static char last_pkg[256];
static char last_sql[4096];

bool t_exec(char *restrict buf, size_t len, const char *restrict file, const char *const argv[]) {
  (void)file;
  if (argv[1] && strcmp(argv[1], "--sqlite") == 0) snprintf(last_sql, sizeof(last_sql), "%s", argv[2]);

  memset(buf, 0, len);
  snprintf(buf, len, "%s", out_text);
  if (out_after_at + strlen(out_after) < len) memcpy(buf + out_after_at, out_after, strlen(out_after));

  return true;
}

uid_t t_uid_from_pkg(const char *restrict pkg) {
  snprintf(last_pkg, sizeof(last_pkg), "%s", pkg);

  return 10123;
}

static int apatch_state(const char *text, const char *after, size_t after_at) {
  out_text = text; out_after = after; out_after_at = after_at;
  setenv("PATH", "/data/adb/ap/bin:/system/bin", 1);

  struct root_impl_state state = { .state = Inexistent };
  apatch_get_existence(&state);

  return state.state;
}

static int magisk_manager_pkg(const char *text, const char *after, size_t after_at, const char *want) {
  out_text = text; out_after = after; out_after_at = after_at;
  last_pkg[0] = '\0';

  magisk_uid_is_manager(10123);
  if (strcmp(last_pkg, want) == 0) return 0;

  printf("manager package looked up: \"%s\", expected \"%s\"\n", last_pkg, want);

  return 1;
}

/* the denylist query for an SQL-escaped name literal */
static void deny_sql(char *buf, size_t size, const char *lit) {
  snprintf(buf, size, "SELECT 1 FROM denylist WHERE process = '%s' OR process || '_zygote' = '%s' OR process || ':' = substr('%s', 1, length(process) + 1) OR (package_name = 'isolated' AND process = substr('%s', 1, length(process))) LIMIT 1", lit, lit, lit, lit);
}

static int magisk_denylist_sql(const char *process, const char *want) {
  out_text = ""; out_after = ""; out_after_at = 0;
  last_sql[0] = '\0';

  magisk_uid_should_umount(process);
  if (strcmp(last_sql, want) == 0) return 0;

  printf("sql:      %s\nexpected: %s\n", last_sql, want);

  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "apatch_version") == 0) return apatch_state("apd 10763", "", 0) == Supported ? 0 : 1;
  if (strcmp(argv[1], "apatch_empty_output") == 0) {
    int state = apatch_state("", "10763", 4);
    if (state == Abnormal) return 0;

    printf("empty apd -V output gave state %d\n", state);

    return 1;
  }
  if (strcmp(argv[1], "magisk_requester") == 0) return magisk_manager_pkg("value=com.example.hidden", "", 0, "com.example.hidden");
  if (strcmp(argv[1], "magisk_sql_plain") == 0) {
    char want[4096];
    deny_sql(want, sizeof(want), "com.example.app:remote");

    return magisk_denylist_sql("com.example.app:remote", want);
  }
  if (strcmp(argv[1], "magisk_sql_quotes") == 0) {
    char want[4096];
    deny_sql(want, sizeof(want), "com.example:a\"b''c");

    return magisk_denylist_sql("com.example:a\"b'c", want);
  }
  if (strcmp(argv[1], "magisk_sql_max_quotes") == 0) {
    /* longest name zygiskd accepts (256 bytes), every byte a quote */
    char name[257], lit[513], want[4096];
    memset(name, '\'', 256); name[256] = '\0';
    memset(lit, '\'', 512); lit[512] = '\0';
    deny_sql(want, sizeof(want), lit);

    return magisk_denylist_sql(name, want);
  }
  /* the query built for $TEST_PROCESS, for tests/run.sh to run against a denylist */
  if (strcmp(argv[1], "magisk_sql_print") == 0) {
    out_text = ""; out_after = ""; out_after_at = 0;
    magisk_uid_should_umount(getenv("TEST_PROCESS"));
    printf("%s\n", last_sql);

    return 0;
  }
  if (strcmp(argv[1], "apatch_isolated_prefix") == 0) return apatch_isolated_prefix();
  if (strcmp(argv[1], "magisk_short_output") == 0) return magisk_manager_pkg("abc", "com.example.stale", 6, "com.topjohnwu.magisk");

  return 2;
}
