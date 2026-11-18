/* Host tests for loader/src/injector/hook.c, built against the test-only
   tests/c/include/jni.h. JNIEnv is a fake function table, the module
   loader, PLT hook library and daemon calls are stand-ins, and
   pthread_mutex_lock can run another registration first, the way a
   second thread could between the capacity check and the lock. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#undef sigmask

static int t_mutex_lock(pthread_mutex_t *m);
#define pthread_mutex_lock(m) t_mutex_lock(m)

#include "../../loader/src/injector/hook.c"

#undef pthread_mutex_lock

/* link stand-ins for what the tested functions do not reach */
void free_maps(struct maps_info *maps) { (void)maps; }
bool plti_add_hook(struct plti *c, const char *l, const char *n, void *cb, void **b) { (void)c; (void)l; (void)n; (void)cb; (void)b; return false; }
bool plti_deinit(struct plti *c) { (void)c; return true; }
bool plti_add_lib(struct plti *c, const char *l) { (void)c; (void)l; return false; }
int parse_int(const char *s) { return atoi(s); }
bool rezygiskd_update_mns(enum mount_namespace_state s, char *b, size_t n) { (void)s; (void)b; (void)n; return false; }
uint32_t rezygiskd_get_process_flags(uid_t u, const char *const p) { (void)u; (void)p; return 0; }
int rezygiskd_get_module_dir(size_t i) { (void)i; return -1; }
int rezygiskd_connect_companion(size_t i) { (void)i; return -1; }
const char *read_std_string(const void *p) { (void)p; return ""; }
bool plti_remove_hook_by_prefix(struct plti *c, const char *l, const char *n, void **o) { (void)c; (void)l; (void)n; (void)o; return false; }
bool plti_remove_hook(struct plti *c, const char *l, const char *n, void **o) { (void)c; (void)l; (void)n; (void)o; return false; }
bool plti_init(struct plti *c) { (void)c; return false; }
bool plti_add_manual_lib(struct plti *c, const char *p, uintptr_t b) { (void)c; (void)p; (void)b; return false; }
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

bool csoloader_load(struct csoloader *lib, const char *lib_path) { (void)lib; return strstr(lib_path, "broken") == NULL; }
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

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);

  if (strcmp(argv[1], "remove_failed_modules") == 0) return test_remove_failed_modules();
  if (strcmp(argv[1], "jni_table_full") == 0) return test_jni_table_full();
  if (strcmp(argv[1], "register_table_race") == 0) return table_race(false);
  if (strcmp(argv[1], "exclude_table_race") == 0) return table_race(true);

  return 2;
}
