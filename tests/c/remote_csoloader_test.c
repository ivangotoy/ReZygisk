/* Host tests for loader/src/ptracer/remote_csoloader.c. The remote
   process is not real: remote syscalls and remote writes are stand-ins
   that record what they are asked to do. ELF fixtures are written at
   run time from made-up values. Host builds exercise the x86_64 paths. */
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <unistd.h>

#define sigabbrev_np t_sigabbrev_np
#define sys_signame t_sys_signame
static const char *t_sys_signame[65];

/* malloc: fresh blocks hold a fixed non-zero pattern, standing in for
   whatever an uninitialized block holds */
static void *t_malloc(size_t size);
#define malloc(n) t_malloc(n)

#include "../../loader/src/ptracer/remote_csoloader.c"

#undef malloc

static void *t_malloc(size_t size) {
  void *p = malloc(size);
  if (p) memset(p, 0x11, size);

  return p;
}

/* remote side: mmap answers from a script, everything is counted */
static long mmap_answers[8];
static int mmap_answer_count = 0, mmap_calls = 0, other_calls = 0;

long remote_syscall(int pid, struct user_regs_struct *regs, uintptr_t gadget, long sysnr, long *args, size_t args_size) {
  (void)pid; (void)regs; (void)gadget; (void)args_size;

  if (sysnr == SYS_openat) return 7;
  if (sysnr == SYS_mmap) {
    long answer = mmap_calls < mmap_answer_count ? mmap_answers[mmap_calls] : args[0];
    mmap_calls++;

    return answer;
  }
  if (sysnr != SYS_close && sysnr != SYS_munmap) other_calls++;

  return 0;
}

uintptr_t find_syscall_gadget(int pid, struct maps_info *remote_map) { (void)pid; (void)remote_map; return 0x1000; }
ssize_t write_proc(int pid, uintptr_t remote_addr, const void *buf, size_t len) { (void)pid; (void)remote_addr; (void)buf; other_calls++; return (ssize_t)len; }

ssize_t read_proc(int pid, uintptr_t remote_addr, void *buf, size_t len) { (void)pid; (void)remote_addr; memset(buf, 0, len); return (ssize_t)len; }
const char *position_after(const char *str, const char needle) { const char *p = strrchr(str, needle); return p ? p + 1 : str; }
void *find_func_addr(struct maps_info *local_info, struct maps_info *remote_info, const char *module, const char *func) {
  (void)local_info; (void)remote_info; (void)module; (void)func;

  return NULL;
}

static char fixture[PATH_MAX];

/* ELF file with one read-only PT_LOAD of <memsz> bytes, <filesz> of them
   in the file; truncated to <cut> bytes when cut > 0 */
static int write_elf(size_t filesz, size_t memsz, size_t cut) {
  const char *dir = getenv("TEST_DIR");
  if (dir == NULL) return -1;
  mkdir(dir, 0700);
  snprintf(fixture, sizeof(fixture), "%s/libexample.so", dir);

  ElfW(Ehdr) eh = { 0 };
  memcpy(eh.e_ident, ELFMAG, SELFMAG);
  eh.e_ident[EI_CLASS] = sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32;
  eh.e_type = ET_DYN;
  eh.e_phoff = sizeof(eh);
  eh.e_phentsize = sizeof(ElfW(Phdr));
  eh.e_phnum = 1;

  ElfW(Phdr) ph = { 0 };
  ph.p_type = PT_LOAD;
  ph.p_flags = PF_R;
  ph.p_offset = 0;
  ph.p_vaddr = 0;
  ph.p_filesz = filesz;
  ph.p_memsz = memsz;

  char buf[8192] = { 0 };
  memcpy(buf, &eh, sizeof(eh));
  memcpy(buf + sizeof(eh), &ph, sizeof(ph));
  size_t len = cut ? cut : (filesz > sizeof(eh) + sizeof(ph) ? filesz : sizeof(eh) + sizeof(ph));

  FILE *f = fopen(fixture, "w");
  if (f == NULL) return -1;
  fwrite(buf, 1, len, f);
  fclose(f);

  return 0;
}

/* program headers cut short by the end of the file: the layout is refused */
static int test_short_phdr_read(void) {
  if (write_elf(0x100, 0x2000, sizeof(ElfW(Ehdr)) + 8) == -1) return 2;

  int fd = open(fixture, O_RDONLY);
  ElfW(Ehdr) eh;
  ElfW(Phdr) *phdr = NULL;
  ElfW(Addr) min_vaddr = 0;
  size_t map_size = 0;

  bool ok = compute_load_layout(fd, 4096, &eh, &phdr, &min_vaddr, &map_size);
  close(fd);
  if (!ok) return 0;

  printf("layout accepted from a cut program header (map size %zu)\n", map_size);

  return 1;
}

/* remote mmap answering -ENOMEM: nothing more is mapped after it */
static int remote_mmap_fails(int failing_call) {
  if (write_elf(0x800, 0x3000, 0) == -1) return 2;

  for (int i = 0; i < 8; i++) mmap_answers[i] = 0x100000000L + i * 0x10000L;
  mmap_answers[failing_call] = -ENOMEM;
  mmap_answer_count = 8;

  struct user_regs_struct regs = { 0 };
  regs.REG_SP = 0x7ff000;
  struct maps_info empty = { 0 };
  uintptr_t base = 0, entry = 0;
  size_t size = 0;

  bool ok = remote_csoloader_load_and_resolve_entry(1234, &regs, &empty, &empty, fixture, &base, &size, &entry);
  if (!ok && mmap_calls == failing_call + 1) return 0;

  printf("returned %d after %d remote mmap call(s); expected failure right after call %d\n", ok, mmap_calls, failing_call + 1);

  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;

  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(10);

  if (strcmp(argv[1], "short_phdr_read") == 0) return test_short_phdr_read();
  if (strcmp(argv[1], "reserve_mmap_errno") == 0) return remote_mmap_fails(0);
  if (strcmp(argv[1], "segment_mmap_errno") == 0) return remote_mmap_fails(1);

  return 2;
}
