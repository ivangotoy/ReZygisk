/* Host test for the state.json writer of loader/src/ptracer/monitor.c.
   module.prop and state.json are redirected to $TEST_DIR; bionic-only
   names used by ptracer/utils.h get host stand-ins. */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define sigabbrev_np t_sigabbrev_np
#define sys_signame t_sys_signame
static const char *t_sys_signame[65];

#ifndef ZKSU_VERSION
#define ZKSU_VERSION "test"
#endif

static FILE *t_fopen(const char *path, const char *mode);
#define fopen(p, m) t_fopen(p, m)

#include "../../loader/src/ptracer/monitor.c"

int fork_dont_care() { return -1; }
void parse_status(int status, char *buf, size_t len) { (void)status; snprintf(buf, len, "test"); }
int get_program(int pid, char *buf, size_t size) { (void)pid; (void)size; buf[0] = '\0'; return -1; }

static char state_path[PATH_MAX];
static char prop_path[PATH_MAX];

static FILE *t_fopen(const char *path, const char *mode) {
  if (strcmp(path, "/data/adb/rezygisk/state.json") == 0) path = state_path;
  else if (strcmp(path, "/data/adb/modules/rezygisk/module.prop") == 0) path = prop_path;

  return (fopen)(path, mode);
}

/* a ',' whose next non-blank character closes an object or array is a
   trailing comma, which JSON.parse rejects */
static int has_trailing_comma(const char *s) {
  for (const char *p = s; *p; p++) {
    if (*p != ',') continue;

    const char *q = p + 1;
    while (*q == ' ' || *q == '\n' || *q == '\t' || *q == '\r') q++;
    if (*q == '}' || *q == ']') return 1;
  }

  return 0;
}

static int test_state_json_stop_reason(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  mkdir(dir, 0700);
  snprintf(state_path, sizeof(state_path), "%s/state.json", dir);
  snprintf(prop_path, sizeof(prop_path), "%s/module.prop", dir);

  environment_information64.root_impl = "Magisk";
  tracing_state = STOPPED;
  monitor_stop_reason = "user requested";
  status64.supported = true;
  status64.daemon_running = true;
  status64.zygote_injected = true;

  if (!update_status(NULL)) return 2;

  FILE *f = (fopen)(state_path, "r");
  if (f == NULL) return 2;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = '\0';
  (fclose)(f);

  if (strstr(buf, "\"reason\": \"user requested\"") == NULL || has_trailing_comma(buf)) {
    printf("%s", buf);

    return 1;
  }

  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "state_json_stop_reason") == 0) return test_state_json_stop_reason();

  return 2;
}
