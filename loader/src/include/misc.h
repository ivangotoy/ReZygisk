#ifndef MISC_H
#define MISC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __LP64__
  #define LP_SELECT(lp32, lp64) lp64
#else
  #define LP_SELECT(lp32, lp64) lp32
#endif

/* INFO: Time limits (milliseconds; a build may override them with -D) */
/* INFO: One request of the loader to ReZygiskd */
#ifndef ZYGISKD_PEER_TIMEOUT_MS
  #define ZYGISKD_PEER_TIMEOUT_MS 10000
#endif
/* INFO: One wait of the tracer or the monitor for a traced process to stop */
#ifndef REZYGISK_TRACE_TIMEOUT_MS
  #define REZYGISK_TRACE_TIMEOUT_MS 10000
#endif
/* INFO: A remote call runs code in zygote, which itself may wait on ReZygiskd
           (up to 4 s of connect retries and one request). Kept below the 30 s
           window in which the monitor counts zygote restarts. */
#ifndef REZYGISK_REMOTE_CALL_TIMEOUT_MS
  #define REZYGISK_REMOTE_CALL_TIMEOUT_MS (REZYGISK_TRACE_TIMEOUT_MS + ZYGISKD_PEER_TIMEOUT_MS)
#endif

#define APP_ID(uid) uid % 100000

#define IS_ISOLATED_SERVICE(uid)      \
  ((APP_ID(uid)) >= 90000)

struct kernel_version {
  uint8_t major;
  unsigned int minor;
  unsigned int patch;
};

struct map_entry {
  uintptr_t start;
  uintptr_t end;
  int perms;
  bool is_private;
  uintptr_t offset;
  dev_t dev;
  ino_t inode;
  char *path;
};

struct maps_info {
  struct map_entry *maps;
  size_t length;
};

int parse_int(const char *str);

struct kernel_version parse_kversion();

struct maps_info *parse_maps(const char *pid);

void free_maps(struct maps_info *maps);

#endif /* MISC_H */
