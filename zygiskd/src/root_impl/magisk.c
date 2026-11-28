#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>

#include "magisk.h"

#include "../utils.h"
#include "common.h"

#define SBIN_MAGISK LP_SELECT("/sbin/magisk32", "/sbin/magisk64")
#define BITLESS_SBIN_MAGISK "/sbin/magisk"
#define DEBUG_RAMDISK_MAGISK LP_SELECT("/debug_ramdisk/magisk32", "/debug_ramdisk/magisk64")
#define BITLESS_DEBUG_RAMDISK_MAGISK "/debug_ramdisk/magisk"

/* INFO: Longest path */
static char path_to_magisk[sizeof(DEBUG_RAMDISK_MAGISK)] = { 0 };

void magisk_get_existence(struct root_impl_state *state) {
  const char *magisk_files[] = {
    SBIN_MAGISK,
    BITLESS_SBIN_MAGISK,
    DEBUG_RAMDISK_MAGISK,
    BITLESS_DEBUG_RAMDISK_MAGISK
  };

  for (size_t i = 0; i < sizeof(magisk_files) / sizeof(magisk_files[0]); i++) {
    if (access(magisk_files[i], F_OK) != 0) continue;

    strcpy(path_to_magisk, magisk_files[i]);

    break;
  }

  if (path_to_magisk[0] == '\0') {
    state->state = Inexistent;

    return;
  }

  const char *argv[] = { "magisk", "-V", NULL };

  char magisk_version[32];
  if (!exec_command(magisk_version, sizeof(magisk_version), (const char *)path_to_magisk, argv)) {
    LOGE("Failed to execute magisk binary: %s", strerror(errno));

    state->state = Abnormal;

    return;
  }

  if (atoi(magisk_version) >= MIN_MAGISK_VERSION) state->state = Supported;
  else state->state = TooOld;
}

bool magisk_uid_granted_root(uid_t uid) {
  char sqlite_cmd[256];
  snprintf(sqlite_cmd, sizeof(sqlite_cmd), "select 1 from policies where uid=%d and policy=2 limit 1", uid);

  const char *const argv[] = { "magisk", "--sqlite", sqlite_cmd, NULL };

  char result[32];
  if (!exec_command(result, sizeof(result), (const char *)path_to_magisk, argv)) {
    LOGE("Failed to execute magisk binary: %s", strerror(errno));

    return false;
  }

  return result[0] != '\0';
}

bool magisk_uid_should_umount(const char *const process) {
  /* INFO: The process name goes into an SQL string literal, so every ' is doubled.
             PROCESS_NAME_MAX_LEN already has a +1 for NULL. */
  char escaped[2 * (PROCESS_NAME_MAX_LEN)];
  size_t escaped_len = 0;
  for (size_t i = 0; process[i] != '\0' && escaped_len + 2 < sizeof(escaped); i++) {
    if (process[i] == '\'') escaped[escaped_len++] = '\'';
    escaped[escaped_len++] = process[i];
  }
  escaped[escaped_len] = '\0';

  char sqlite_cmd[160 + 3 * 2 * (PROCESS_NAME_MAX_LEN)];
  /* INFO: An entry matches the process itself, its "<entry>:<suffix>" processes and its
             "<entry>_zygote" app zygote; never a longer name that merely starts with it
             (com.foo2 for com.foo). Compared with = and substr(), never LIKE, so a % or _
             in an entry or in the name is an ordinary character. */
  snprintf(sqlite_cmd, sizeof(sqlite_cmd),
    "SELECT 1 FROM denylist WHERE process = '%s' OR process || '_zygote' = '%s' OR process || ':' = substr('%s', 1, length(process) + 1) LIMIT 1",
    escaped, escaped, escaped);

  const char *const argv[] = { "magisk", "--sqlite", sqlite_cmd, NULL };

  char result[sizeof("1=1")];
  if (!exec_command(result, sizeof(result), (const char *)path_to_magisk, argv)) {
    LOGE("Failed to execute magisk binary: %s", strerror(errno));

    return false;
  }

  return result[0] != '\0';
}

bool magisk_uid_is_manager(uid_t uid) {
  const char *const argv[] = { "magisk", "--sqlite", "select value from strings where key=\"requester\" limit 1", NULL };

  char output[128];
  if (!exec_command(output, sizeof(output), (const char *)path_to_magisk, argv)) {
    LOGE("Failed to execute magisk binary: %s", strerror(errno));

    return false;
  }

  char pkg[NAME_MAX + 1] = "com.topjohnwu.magisk";
  if (strncmp(output, "value=", strlen("value=")) == 0)
    snprintf(pkg, sizeof(pkg), "%s", output + strlen("value="));

  uid_t manager_uid = uid_from_pkg(pkg);
  if (!manager_uid) {
    LOGE("Failed to retrieve uid for %s", pkg);

    return false;
  }

  return manager_uid == APP_ID(uid);
}
