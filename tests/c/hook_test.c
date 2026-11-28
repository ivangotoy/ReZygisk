/* Host tests for loader/src/injector/hook.c, built against the test-only
   tests/c/include/jni.h. JNIEnv is a fake function table, the module
   loader, PLT hook library and daemon calls are stand-ins, and
   pthread_mutex_lock can run another registration first, the way a
   second thread could between the capacity check and the lock. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#undef sigmask

/* malloc: with poison_malloc set, a new module table holds stale module
   entries like reused heap memory */
static bool poison_malloc = false;
static void *t_malloc(size_t size);
#define malloc(n) t_malloc(n)

static int t_mutex_lock(pthread_mutex_t *m);
#define pthread_mutex_lock(m) t_mutex_lock(m)

#include "../../loader/src/injector/hook.c"

#undef pthread_mutex_lock
#undef malloc

/* a hook left in stale memory: reaching it ends the process */
static void stale_hook(void *impl, const void *args) { (void)impl; (void)args; _exit(3); }

/* a block the size of a module table is filled like a freed table of
   registered API v5 modules (the stale bytes a reused block can hold) */
static void *t_malloc(size_t size) {
  void *p = malloc(size);
  if (p && poison_malloc && size > 0 && size % sizeof(struct rezygisk_module) == 0) {
    for (struct rezygisk_module *m = p; (char *)m < (char *)p + size; m++) {
      memset(m, 0x5a, sizeof(*m));
      m->abi.api_version = 5;
      m->abi.pre_app_specialize = (void *)stale_hook;
      m->abi.post_app_specialize = (void *)stale_hook;
    }
  }

  return p;
}

/* link stand-ins for what the tested functions do not reach */
void free_maps(struct maps_info *maps) { (void)maps; }
/* PLTI stand-ins: a hook succeeds only in a library added before, the
   way plti_add_hook only finds libraries in its list; an added library
   must start with an ELF header in this process */
static char plti_known[64][PATH_MAX];
static int plti_known_count = 0;
static int plti_not_elf = 0;
bool plti_add_hook(struct plti *c, const char *l, const char *n, void *cb, void **b) {
  (void)c; (void)n; (void)cb; (void)b;
  for (int i = 0; i < plti_known_count; i++) if (strcmp(plti_known[i], l) == 0) return true;

  return false;
}
bool plti_deinit(struct plti *c) { (void)c; return true; }
bool plti_add_lib(struct plti *c, const char *l) { (void)c; (void)l; return false; }
int parse_int(const char *s) { return atoi(s); }
bool rezygiskd_update_mns(enum mount_namespace_state s, char *b, size_t n) { (void)s; (void)b; (void)n; return false; }
static int flags_requests = 0;
uint32_t rezygiskd_get_process_flags(uid_t u, const char *const p) { (void)u; (void)p; flags_requests++; return 0; }
int rezygiskd_get_module_dir(size_t i) { (void)i; return -1; }
int rezygiskd_connect_companion(size_t i) { (void)i; return -1; }
const char *read_std_string(const void *p) { (void)p; return ""; }
bool plti_remove_hook_by_prefix(struct plti *c, const char *l, const char *n, void **o) { (void)c; (void)l; (void)n; (void)o; return false; }
bool plti_remove_hook(struct plti *c, const char *l, const char *n, void **o) { (void)c; (void)l; (void)n; (void)o; return false; }
bool plti_init(struct plti *c) { (void)c; return false; }
bool plti_add_manual_lib(struct plti *c, const char *p, uintptr_t b) {
  (void)c;
  if (b == 0 || memcmp((const void *)b, "\177ELF", 4) != 0) { plti_not_elf++; return false; }
  if (plti_known_count < 64) snprintf(plti_known[plti_known_count++], PATH_MAX, "%s", p);

  return true;
}
bool plti_add_hook_by_prefix(struct plti *c, const char *l, const char *n, void *cb, void **b) { (void)c; (void)l; (void)n; (void)cb; (void)b; return false; }
void csoloader_deinit(void) {}
bool csoloader_abandon(struct csoloader *lib) { (void)lib; return true; }

/* ---- module loading (F4): three modules, the first two fail to load ---- */
static char module_paths[3][PATH_MAX];
static size_t removed[8];
static int removed_count = 0;

bool rezygiskd_read_modules(struct zygisk_modules *modules) {
  modules->modules_count = 3;
  modules->modules = malloc(3 * sizeof(char *));
  for (int i = 0; i < 3; i++) modules->modules[i] = strdup(module_paths[i]);

  return true;
}

void free_modules(struct zygisk_modules *modules) {
  for (size_t i = 0; i < modules->modules_count; i++) free(modules->modules[i]);
  free(modules->modules);
}

static struct csoloader_elf test_img = { .elf = "test.so" };
bool csoloader_load(struct csoloader *lib, const char *lib_path) { lib->img = &test_img; return strstr(lib_path, "broken") == NULL; }
bool csoloader_unload(struct csoloader *lib) { (void)lib; return true; }
static void fake_entry(void *a, void *b) { (void)a; (void)b; }
void *csoloader_get_symbol(struct csoloader *lib, const char *name) { (void)lib; (void)name; return (void *)fake_entry; }

/* the daemon closes the gap after each removal, like zygiskd.c does */
bool rezygiskd_remove_module(size_t index) {
  removed[removed_count++] = index;

  return true;
}

static int test_remove_failed_modules(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  mkdir(dir, 0700);

  const char *names[3] = { "broken_a.so", "broken_b.so", "working.so" };
  for (int i = 0; i < 3; i++) {
    snprintf(module_paths[i], PATH_MAX, "%s/%s", dir, names[i]);
    FILE *f = fopen(module_paths[i], "w");
    if (f == NULL) return 2;
    fclose(f);
  }

  if (!load_modules_only()) return 2;

  /* daemon list [a, b, w]: removing a leaves [b, w], so b is index 0 too */
  if (removed_count == 2 && removed[0] == 0 && removed[1] == 0 && zygisk_module_length == 1) return 0;

  printf("removed %d module(s): indices", removed_count);
  for (int i = 0; i < removed_count; i++) printf(" %zu", removed[i]);
  printf(", %zu loaded\n", zygisk_module_length);

  return 1;
}

/* ---- a module whose entry never registers: its pre/post specialize
   hooks are absent, not whatever was left in the module table ---- */
static int test_unregistered_module_skipped(void) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return 2;
  mkdir(dir, 0700);

  const char *names[3] = { "broken_a.so", "broken_b.so", "working.so" };
  for (int i = 0; i < 3; i++) {
    snprintf(module_paths[i], PATH_MAX, "%s/%s", dir, names[i]);
    FILE *f = fopen(module_paths[i], "w");
    if (f == NULL) return 2;
    fclose(f);
  }

  poison_malloc = true;
  bool loaded = load_modules_only();
  poison_malloc = false;
  if (!loaded || zygisk_module_length != 1) return 2;

  /* in a child: calling a stale function pointer crashes it */
  pid_t pid = fork();
  if (pid == 0) {
    struct app_specialize_args_v5 args;
    memset(&args, 0, sizeof(args));
    struct zygisk_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.args.app = &args;
    FLAG_SET(&ctx, APP_SPECIALIZE);

    rz_run_modules_pre(&ctx);
    rz_run_modules_post(&ctx);

    _exit(0);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

  printf("specializing with an unregistered module ended the process: status 0x%x\n", status);

  return 1;
}

/* ---- an isolated service without an app data dir (null jstring) ---- */
/* ART aborts the process when GetStringUTFChars gets a null string */
static const char *t_get_string_utf_chars(JNIEnv *env, jstring s, jboolean *copy) {
  (void)env; (void)copy;
  if (s == NULL) _exit(4);

  return "/data/user/0/com.example.app";
}
static void t_release_string_utf_chars(JNIEnv *env, jstring s, const char *c) { (void)env; (void)s; (void)c; }

static int test_isolated_without_data_dir(void) {
  pid_t pid = fork();
  if (pid == 0) {
    struct JNINativeInterface table;
    memset(&table, 0, sizeof(table));
    table.GetStringUTFChars = t_get_string_utf_chars;
    table.ReleaseStringUTFChars = t_release_string_utf_chars;
    JNIEnv env = &table;

    jint uid = 99000;
    jstring data_dir = NULL;
    struct app_specialize_args_v5 args;
    memset(&args, 0, sizeof(args));
    args.uid = &uid;
    args.app_data_dir = &data_dir;

    struct zygisk_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.env = &env;
    ctx.args.app = &args;
    ctx.process = "com.example.app:isolated";

    rz_app_specialize_pre(&ctx);

    /* the process is still specialized: its flags were asked for */
    _exit(flags_requests == 1 ? 0 : 5);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

  printf("isolated service without a data dir: status 0x%x (0x400: null string read, 0x500: no flags request)\n", status);

  return 1;
}

/* ---- the process name taken before an app fork is released once in
   zygote too ---- */
static int utf_gets = 0, utf_releases = 0;
static const char *t_counting_get(JNIEnv *env, jstring s, jboolean *copy) { (void)env; (void)s; (void)copy; utf_gets++; return "com.example.app"; }
static void t_counting_release(JNIEnv *env, jstring s, const char *c) { (void)env; (void)s; (void)c; utf_releases++; }
static int t_fork_parent(void) { return 12345; }

static int test_fork_parent_releases_name(void) {
  struct JNINativeInterface table;
  memset(&table, 0, sizeof(table));
  table.GetStringUTFChars = t_counting_get;
  table.ReleaseStringUTFChars = t_counting_release;
  JNIEnv env = &table;

  jint uid = 10123;
  jstring nice_name = (jstring)&uid;
  struct app_specialize_args_v5 args;
  memset(&args, 0, sizeof(args));
  args.uid = &uid;
  args.nice_name = &nice_name;

  old_fork = t_fork_parent;

  struct zygisk_context ctx;
  rz_init(&ctx, &env, &args);
  rz_nativeForkAndSpecialize_pre(&ctx);
  rz_nativeForkAndSpecialize_post(&ctx);
  rz_cleanup(&ctx);

  if (utf_gets == 1 && utf_releases == 1) return 0;

  printf("zygote side of an app fork: %d GetStringUTFChars, %d ReleaseStringUTFChars\n", utf_gets, utf_releases);

  return 1;
}

/* ---- a fork that fails in nativeForkAndSpecialize/nativeForkSystemServer:
   zygote stays zygote, the native fork gets the failure ---- */
static int fork_calls = 0;
static int t_fork_fails(void) { fork_calls++; errno = EAGAIN; return -1; }
static int entry_calls = 0;
static void counting_entry(void *api, void *env) { (void)api; (void)env; entry_calls++; }

static bool sigchld_blocked(void) {
  sigset_t set;
  sigprocmask(SIG_BLOCK, NULL, &set);

  return sigismember(&set, SIGCHLD) == 1;
}

/* exit codes: 0 ok, 1 forked twice, 2 wrong fork result, 3 ran as child,
   4 name not paired, 5 zygisk would unmap, 6 SIGCHLD left blocked */
static int fork_failure_case(bool server) {
  struct JNINativeInterface table;
  memset(&table, 0, sizeof(table));
  table.GetStringUTFChars = t_counting_get;
  table.ReleaseStringUTFChars = t_counting_release;
  JNIEnv env = &table;

  jint uid = 10123;
  jstring nice_name = (jstring)&uid;
  jstring data_dir = NULL;
  struct app_specialize_args_v5 app;
  memset(&app, 0, sizeof(app));
  app.uid = &uid;
  app.nice_name = &nice_name;
  app.app_data_dir = &data_dir;
  struct server_specialize_args_v1 srv;
  memset(&srv, 0, sizeof(srv));
  srv.uid = &uid;

  /* one loaded module: its entry runs only when modules are run */
  zygisk_modules = calloc(1, sizeof(struct rezygisk_module));
  zygisk_modules[0].zygisk_module_entry = counting_entry;
  zygisk_modules[0].lib.img = &test_img;
  zygisk_module_length = 1;

  old_fork = t_fork_fails;

  struct zygisk_context ctx;
  rz_init(&ctx, &env, server ? (void *)&srv : (void *)&app);
  if (server) rz_nativeForkSystemServer_pre(&ctx);
  else rz_nativeForkAndSpecialize_pre(&ctx);

  /* the original native's fork() */
  errno = 0;
  int ret = new_fork();
  int ret_errno = errno;

  if (server) rz_nativeForkSystemServer_post(&ctx);
  else rz_nativeForkAndSpecialize_post(&ctx);
  rz_cleanup(&ctx);

  printf("fork calls %d, native fork %d errno %d, flags requests %d, module entries %d, name %d/%d, unmap %d, SIGCHLD blocked %d\n",
         fork_calls, ret, ret_errno, flags_requests, entry_calls, utf_gets, utf_releases, should_unmap_zygisk, sigchld_blocked());

  if (fork_calls != 1) return 1;
  if (ret != -1 || ret_errno != EAGAIN) return 2;
  if (flags_requests != 0 || entry_calls != 0) return 3;
  if (!server && (utf_gets != 1 || utf_releases != 1)) return 4;
  if (should_unmap_zygisk) return 5;
  if (sigchld_blocked()) return 6;

  return 0;
}

static int test_fork_fails(bool server) {
  pid_t pid = fork();
  if (pid == 0) _exit(fork_failure_case(server));

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

  printf("failed %s fork: status 0x%x\n", server ? "system_server" : "app", status);

  return 1;
}

/* specializing in place (no fork) is still the child: flags are asked for */
static int test_specialize_in_place(void) {
  pid_t pid = fork();
  if (pid == 0) {
    struct JNINativeInterface table;
    memset(&table, 0, sizeof(table));
    table.GetStringUTFChars = t_counting_get;
    table.ReleaseStringUTFChars = t_counting_release;
    JNIEnv env = &table;

    jint uid = 10123;
    jstring nice_name = (jstring)&uid;
    jstring data_dir = NULL;
    struct app_specialize_args_v5 app;
    memset(&app, 0, sizeof(app));
    app.uid = &uid;
    app.nice_name = &nice_name;
    app.app_data_dir = &data_dir;

    struct zygisk_context ctx;
    rz_init(&ctx, &env, &app);
    rz_nativeSpecializeAppProcess_pre(&ctx);

    _exit(flags_requests == 1 ? 0 : 1);
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;

  printf("specialize in place: status 0x%x\n", status);

  return 1;
}

/* ---- JNI hooks (R15 :432): more methods than the 32-entry table ---- */
#define NMETHODS 33
static void *orig_fns[NMETHODS];
static void *art_methods[NMETHODS][1];
static int registered = -1;

static jclass f_FindClass(JNIEnv *e, const char *n) { (void)e; (void)n; return (jclass)1; }
static jmethodID f_GetMethodID(JNIEnv *e, jclass c, const char *n, const char *s) { (void)e; (void)c; (void)s; return (jmethodID)(uintptr_t)(atoi(n + 1) + 1); }
static jmethodID f_GetStaticMethodID(JNIEnv *e, jclass c, const char *n, const char *s) { (void)e; (void)c; (void)n; (void)s; return NULL; }
static jobject f_ToReflectedMethod(JNIEnv *e, jclass c, jmethodID m, jboolean st) { (void)e; (void)c; (void)st; return (jobject)m; }
static jint f_CallIntMethod(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; (void)o; (void)m; return 0x100; }
static jboolean f_ExceptionCheck(JNIEnv *e) { (void)e; return 0; }
static void f_ExceptionClear(JNIEnv *e) { (void)e; }
static void f_DeleteLocalRef(JNIEnv *e, jobject o) { (void)e; (void)o; }
static jlong f_GetLongField(JNIEnv *e, jobject o, jfieldID f) { (void)e; (void)f; return (jlong)(uintptr_t)art_methods[(uintptr_t)o - 1]; }
static jint f_RegisterNatives(JNIEnv *e, jclass c, const JNINativeMethod *m, jint n) { (void)e; (void)c; (void)m; registered = n; return 0; }

static int test_jni_table_full(void) {
  struct JNINativeInterface table = {
    .FindClass = f_FindClass, .GetMethodID = f_GetMethodID, .GetStaticMethodID = f_GetStaticMethodID,
    .ToReflectedMethod = f_ToReflectedMethod, .CallIntMethod = f_CallIntMethod, .ExceptionCheck = f_ExceptionCheck,
    .ExceptionClear = f_ExceptionClear, .DeleteLocalRef = f_DeleteLocalRef, .GetLongField = f_GetLongField,
    .RegisterNatives = f_RegisterNatives
  };
  JNIEnv env_table = &table;
  JNIEnv *env = &env_table;

  can_hook_jni = true;
  MODIFIER_NATIVE = 0x100;
  art_method_field = (jfieldID)1;
  data_offset = 0;

  static char names[NMETHODS][8];
  JNINativeMethod methods[NMETHODS];
  for (int i = 0; i < NMETHODS; i++) {
    orig_fns[i] = (void *)(uintptr_t)(0x1000 + i);
    art_methods[i][0] = orig_fns[i];
    snprintf(names[i], sizeof(names[i]), "m%d", i);
    methods[i] = (JNINativeMethod){ .name = names[i], .signature = "()V", .fnPtr = (void *)(uintptr_t)(0x9000 + i) };
  }

  hook_jni_methods(env, "com/example/Target", methods, NMETHODS);

  /* the first 32 are hooked (backup = original), the 33rd could not be */
  for (int i = 0; i < 32; i++) {
    if (methods[i].fnPtr != orig_fns[i]) { printf("method %d not hooked\n", i); return 1; }
  }
  if (registered == 32 && methods[32].fnPtr == NULL) return 0;

  printf("registered %d; method 33 reports %p (NULL = not hooked)\n", registered, methods[32].fnPtr);

  return 1;
}

/* ---- PLT hook registration (R15 :668, :685) ---- */
static int (*reenter)(void) = NULL;

static int t_mutex_lock(pthread_mutex_t *m) {
  if (reenter) {
    int (*other)(void) = reenter;
    reenter = NULL;
    other();
  }

  return pthread_mutex_lock(m);
}

static void cb(void) {}
static int register_one(void) { api_plt_hook_register(".*", "symbol", (void *)cb, NULL); return 0; }
static int exclude_one(void) { api_plt_hook_exclude(".*", "symbol"); return 0; }

static int table_race(bool ignore) {
  static struct zygisk_context ctx;
  memset(&ctx, 0, sizeof(ctx));
  pthread_mutex_init(&ctx.hook_info_lock, NULL);
  g_ctx = &ctx;
  if (ignore) ctx.ignore_info_count = MAX_IGNORE_INFO - 1;
  else ctx.register_info_count = MAX_REGISTER_INFO - 1;

  reenter = ignore ? exclude_one : register_one;
  if (ignore) exclude_one();
  else register_one();

  size_t count = ignore ? ctx.ignore_info_count : ctx.register_info_count;
  size_t max = ignore ? MAX_IGNORE_INFO : MAX_REGISTER_INFO;
  if (count == max) return 0;

  printf("%zu entries in a %zu-entry table\n", count, max);

  return 1;
}

/* an API v1-v3 hook by path regex: the matching library is handed to
   PLTI (at its ELF header) before the hook, so the commit succeeds */
static int test_plt_commit_registers_library(void) {
  static struct zygisk_context ctx;
  memset(&ctx, 0, sizeof(ctx));
  pthread_mutex_init(&ctx.hook_info_lock, NULL);
  g_ctx = &ctx;
  zygisk_module_length = 0;

  api_plt_hook_register(".*/libc[.-][^/]*$", "open", (void *)cb, NULL);
  if (ctx.register_info_count != 1) return 2;

  bool ok = api_plt_hook_commit();
  if (ok && plti_known_count > 0 && plti_not_elf == 0) return 0;

  printf("commit %s, %d libraries added, %d not at an ELF header\n", ok ? "ok" : "failed", plti_known_count, plti_not_elf);

  return 1;
}

/* a module loaded by CSOLoader is listed at its loaded image, never at
   the heap copy of its file */
static int test_module_map_uses_loaded_base(void) {
  static char file_copy[64], loaded[64];
  static struct csoloader_elf img;
  img.base = loaded;
  img.header = (ElfW(Ehdr) *)file_copy;
  img.size = sizeof(loaded);

  static struct rezygisk_module mod;
  mod.lib.lib_path = "/data/adb/modules/example/zygisk/arm64-v8a.so";
  mod.lib.img = &img;
  zygisk_modules = &mod;
  zygisk_module_length = 1;

  struct maps_info *maps = get_loaded_libraries();
  if (!maps) return 2;

  for (size_t i = 0; i < maps->length; i++) {
    if (!maps->maps[i].path || strcmp(maps->maps[i].path, mod.lib.lib_path) != 0) continue;
    if (maps->maps[i].start == (uintptr_t)loaded) return 0;

    printf("module listed at %#zx (file copy %p), loaded at %p\n", (size_t)maps->maps[i].start, (void *)file_copy, (void *)loaded);

    return 1;
  }

  printf("module not listed\n");

  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "remove_failed_modules") == 0) return test_remove_failed_modules();
  if (strcmp(argv[1], "unregistered_module_skipped") == 0) return test_unregistered_module_skipped();
  if (strcmp(argv[1], "isolated_without_data_dir") == 0) return test_isolated_without_data_dir();
  if (strcmp(argv[1], "fork_parent_releases_name") == 0) return test_fork_parent_releases_name();
  if (strcmp(argv[1], "app_fork_fails") == 0) return test_fork_fails(false);
  if (strcmp(argv[1], "server_fork_fails") == 0) return test_fork_fails(true);
  if (strcmp(argv[1], "specialize_in_place") == 0) return test_specialize_in_place();
  if (strcmp(argv[1], "jni_table_full") == 0) return test_jni_table_full();
  if (strcmp(argv[1], "register_table_race") == 0) return table_race(false);
  if (strcmp(argv[1], "exclude_table_race") == 0) return table_race(true);
  if (strcmp(argv[1], "module_map_uses_loaded_base") == 0) return test_module_map_uses_loaded_base();
  if (strcmp(argv[1], "plt_commit_registers_library") == 0) return test_plt_commit_registers_library();

  return 2;
}
