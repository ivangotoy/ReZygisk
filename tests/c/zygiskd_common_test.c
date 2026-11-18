/* Host tests for zygiskd/src/root_impl/common.c. /data/user and
   /data/user_de are redirected to a fixture; the per-implementation
   functions it dispatches to are stubbed. */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static DIR *t_opendir(const char *path);
static int t_stat(const char *path, struct stat *st);
#define opendir(p) t_opendir(p)
#define stat(p, s) t_stat(p, s)

#include "../../zygiskd/src/root_impl/common.c"

#undef opendir
#undef stat

void ksu_get_existence(struct root_impl_state *state) { state->state = Inexistent; }
bool ksu_uid_granted_root(uid_t uid) { (void)uid; return false; }
bool ksu_uid_should_umount(uid_t uid) { (void)uid; return false; }
bool ksu_uid_is_manager(uid_t uid) { (void)uid; return false; }
void ksu_cleanup(void) {}
void apatch_get_existence(struct root_impl_state *state) { state->state = Inexistent; }
bool apatch_uid_granted_root(uid_t uid) { (void)uid; return false; }
bool apatch_uid_should_umount(uid_t uid, const char *const process) { (void)uid; (void)process; return false; }
bool apatch_uid_is_manager(uid_t uid) { (void)uid; return false; }
void magisk_get_existence(struct root_impl_state *state) { state->state = Inexistent; }
bool magisk_uid_granted_root(uid_t uid) { (void)uid; return false; }
bool magisk_uid_should_umount(const char *const process) { (void)process; return false; }
bool magisk_uid_is_manager(uid_t uid) { (void)uid; return false; }

/* Users 0 and 10 exist; only user 10 has com.example.manager, owned by uid 1010123 */
#define FIXTURE_PKG "com.example.manager"
#define FIXTURE_UID 1010123

static DIR *t_opendir(const char *path) {
  if (strcmp(path, "/data/user") != 0) return opendir(path);

  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return NULL;

  char users[PATH_MAX];
  snprintf(users, sizeof(users), "%s/user", dir);
  mkdir(dir, 0700);
  mkdir(users, 0700);

  char u[PATH_MAX];
  snprintf(u, sizeof(u), "%s/0", users);
  mkdir(u, 0700);
  snprintf(u, sizeof(u), "%s/10", users);
  mkdir(u, 0700);

  return opendir(users);
}

/* stat() leaves *st indeterminate on failure; model that with a fixed
   non-zero pattern so a read of it is visible */
static int t_stat(const char *path, struct stat *st) {
  if (strcmp(path, "/data/user_de/10/" FIXTURE_PKG) == 0) {
    memset(st, 0, sizeof(*st));
    st->st_uid = FIXTURE_UID;

    return 0;
  }

  memset(st, 0x5a, sizeof(*st));
  errno = ENOENT;

  return -1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "uid_from_pkg_found") == 0) {
    uid_t uid = uid_from_pkg(FIXTURE_PKG);
    if (uid == FIXTURE_UID % 100000) return 0;

    printf("got %u\n", (unsigned)uid);

    return 1;
  }

  if (strcmp(argv[1], "uid_from_pkg_missing") == 0) {
    uid_t uid = uid_from_pkg("com.example.absent");
    if (uid == 0) return 0;

    printf("got %u for a package that is not installed\n", (unsigned)uid);

    return 1;
  }

  return 2;
}
