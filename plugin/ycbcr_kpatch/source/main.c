/*
 * ycbcr_kpatch — GoldHEN BinLoader payload (FW 9.00)
 *
 * MODE 0: ping (default, safe)
 * MODE 1: apply YCBCR_KPATCH_FIXED_OFF
 * MODE 2: scan (dangerous; do not use)
 * MODE 3: dump ONE island [DUMP_OFF, DUMP_OFF+DUMP_LEN) → kernel_900.bin
 *         (4 KiB/page; no full sweep — see README)
 */

#include "ps4.h"
#include "offsets_900.h"

#ifndef MODE
#define MODE 0
#endif

#ifndef DUMP_MAX
#define DUMP_MAX 0x00C00000ull /* 12 MiB */
#endif

struct kpatch_info {
  uint32_t status;
  uint64_t patch_off;
  uint32_t jcc_len;
};

struct kpatch_args {
  void *syscall_handler;
  struct kpatch_info *info;
};

#if MODE >= 1 && MODE < 3
static int is_jcc(const uint8_t *p, int *out_len) {
  if (p[0] == 0x0F && (p[1] & 0xF0) == 0x80) {
    *out_len = 6;
    return 1;
  }
  if (p[0] >= 0x70 && p[0] <= 0x7F) {
    *out_len = 2;
    return 1;
  }
  return 0;
}

static int already_nopped(const uint8_t *p, int len) {
  for (int i = 0; i < len; i++) {
    if (p[i] != 0x90)
      return 0;
  }
  return 1;
}

static int kpatch_apply(struct thread *td, struct kpatch_args *args) {
  (void)td;
  uint8_t *kbase = (uint8_t *)(__readmsr(0xC0000082) - (uint64_t)K900_XFAST_SYSCALL);
  int (*copyout)(const void *kaddr, void *uaddr, size_t len) =
      (void *)(kbase + (uint64_t)K900_COPYOUT);

  struct kpatch_info local = {
      .status = YCBCR_KPATCH_MAGIC_FAIL,
      .patch_off = 0,
      .jcc_len = 0,
  };

  if (!args || !args->info)
    return -1;

  struct kpatch_info *uinfo = args->info;
  uint64_t want_off = uinfo->patch_off;
  uint32_t want_len = uinfo->jcc_len;

  if (!want_off || want_len == 0 || want_len > 6) {
    copyout(&local, uinfo, sizeof(local));
    return -1;
  }

  uint8_t *site = kbase + want_off;
  local.patch_off = want_off;
  local.jcc_len = want_len;

  if (already_nopped(site, (int)want_len)) {
    local.status = YCBCR_KPATCH_MAGIC_ALREADY;
  } else {
    int len = 0;
    if (!is_jcc(site, &len)) {
      local.status = YCBCR_KPATCH_MAGIC_FAIL;
      copyout(&local, uinfo, sizeof(local));
      return -1;
    }
    want_len = (uint32_t)len;
    local.jcc_len = want_len;

    uint64_t cr0 = readCr0();
    writeCr0(cr0 & ~X86_CR0_WP);
    for (uint32_t i = 0; i < want_len; i++)
      site[i] = 0x90;
    writeCr0(cr0);
    local.status = YCBCR_KPATCH_MAGIC_OK;
  }

  copyout(&local, uinfo, sizeof(local));
  return (local.status == YCBCR_KPATCH_MAGIC_OK ||
          local.status == YCBCR_KPATCH_MAGIC_ALREADY)
             ? 0
             : -1;
}
#endif

#if MODE == 2
static const uint8_t P1[] = {0x06, 0x82, 0x30, 0xC0};
static const uint8_t P2[] = {0x00, 0x22, 0x32, 0x08};
static const uint8_t P3[] = {
    0x81, 0xE1, 0x00, 0x00, 0x00, 0xC0,
    0x81, 0xF9, 0x00, 0x00, 0x00, 0x80};

static int memeq(const uint8_t *a, const uint8_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i])
      return 0;
  }
  return 1;
}

static int dump_rel(uint64_t kbase, uint64_t off, void *buf, size_t n) {
  return get_memory_dump(kbase + off, (uint64_t *)buf, n);
}

static int scan_userspace(uint64_t kbase, uint64_t *out_off, int *out_jlen) {
  static uint8_t page[KSCAN_PAGE];
  static uint8_t win[PAIR_WINDOW * 2 + 64];
  *out_off = 0;
  *out_jlen = 0;
  int hits = 0;
  uint64_t best = 0;
  int best_len = 0;

  for (uint64_t off = KSCAN_START; off + KSCAN_PAGE <= KSCAN_END; off += KSCAN_PAGE) {
    memset(page, 0, sizeof(page));
    if (dump_rel(kbase, off, page, KSCAN_PAGE) < 0)
      continue;
    for (uint32_t i = 0; i + 4 < KSCAN_PAGE; i++) {
      if (!memeq(page + i, P1, sizeof(P1)))
        continue;
      uint64_t p1_off = off + i;
      uint64_t lo = (p1_off > PAIR_WINDOW) ? (p1_off - PAIR_WINDOW) : KSCAN_START;
      uint64_t hi = p1_off + PAIR_WINDOW;
      if (hi > KSCAN_END)
        hi = KSCAN_END;
      uint64_t span = hi - lo;
      if (span > sizeof(win))
        span = sizeof(win);
      memset(win, 0, sizeof(win));
      if (dump_rel(kbase, lo, win, (size_t)span) < 0)
        continue;
      for (uint64_t j = 0; j + 12 < span; j++) {
        uint64_t after = 0;
        if (memeq(win + j, P2, sizeof(P2)))
          after = j + 4;
        else if (memeq(win + j, P3, sizeof(P3)))
          after = j + 12;
        else
          continue;
        for (uint64_t d = 0; d < 24 && after + d + 6 < span; d++) {
          int len = 0;
          if (!is_jcc(win + after + d, &len))
            continue;
          hits++;
          if (!best) {
            best = lo + after + d;
            best_len = len;
          }
          break;
        }
      }
    }
  }
  if (!best || hits == 0 || hits > 4)
    return 0;
  *out_off = best;
  *out_jlen = best_len;
  return hits;
}
#endif

int _main(struct thread *td) {
  (void)td;
  initKernel();
  initLibc();
  initSysUtil();

#if MODE == 0
  printf_notification("ycbcr_kpatch ping OK");
  return 0;

#elif MODE == 1
  if (!YCBCR_KPATCH_FIXED_OFF) {
    printf_notification("ycbcr_kpatch: set FIXED_OFF");
    return 1;
  }
  {
    struct kpatch_info info = {0};
    info.patch_off = YCBCR_KPATCH_FIXED_OFF;
    info.jcc_len = 6;
    (void)kexec(&kpatch_apply, &info);
    if (info.status == YCBCR_KPATCH_MAGIC_OK) {
      printf_notification("ycbcr_kpatch OK 0x%llx",
                          (unsigned long long)info.patch_off);
      return 0;
    }
    if (info.status == YCBCR_KPATCH_MAGIC_ALREADY) {
      printf_notification("ycbcr_kpatch already 0x%llx",
                          (unsigned long long)info.patch_off);
      return 0;
    }
    printf_notification("ycbcr_kpatch FAIL st=0x%x", info.status);
    return 1;
  }

#elif MODE == 2
  if (!is_jailbroken())
    jailbreak();
  {
    uint64_t kbase = get_kernel_base();
    uint64_t found_off = 0;
    int found_len = 0;
    if (!kbase || kbase == (uint64_t)-1) {
      printf_notification("ycbcr_kpatch: no kbase");
      return 1;
    }
    if (!scan_userspace(kbase, &found_off, &found_len)) {
      printf_notification("ycbcr_kpatch: no pattern");
      return 1;
    }
    printf_notification("ycbcr_kpatch FOUND 0x%llx len=%d",
                        (unsigned long long)found_off, found_len);
    return 0;
  }

#elif MODE == 3
  /*
   * 4 KiB buffer + write per page. Window:
   *   -DDUMP_OFF=… -DDUMP_LEN=…
   * Do not send anything to :9090 in a loop; one payload at a time.
   */
  if (!is_jailbroken())
    jailbreak();

  {
#ifndef DUMP_OFF
#define DUMP_OFF 0ull
#endif
#ifndef DUMP_LEN
#define DUMP_LEN 0x10000ull /* 64 KiB */
#endif
    enum { PAGE = 0x1000u };

    uint64_t kbase = get_kernel_base();
    if (!kbase || kbase == (uint64_t)-1) {
      printf_notification("ycbcr_kdump: no kbase");
      return 1;
    }

    uint8_t *page = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (!page || page == MAP_FAILED) {
      printf_notification("ycbcr_kdump: mmap fail");
      return 1;
    }
    memset(page, 0, PAGE);
    mlock(page, PAGE);

    if (get_memory_dump(kbase + DUMP_OFF, (uint64_t *)page, 16) < 0) {
      munmap(page, PAGE);
      printf_notification("ycbcr_kdump: probe fail");
      return 1;
    }
    printf_notification("ycbcr_kdump probe @0x%llx",
                        (unsigned long long)DUMP_OFF);

    mkdir("/data/moonlight", 0777);
    int fd = open("/data/moonlight/kernel_900.bin", O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
      munmap(page, PAGE);
      printf_notification("ycbcr_kdump: open fail");
      return 1;
    }
    if (lseek(fd, (off_t)DUMP_OFF, SEEK_SET) < 0) {
      close(fd);
      munmap(page, PAGE);
      printf_notification("ycbcr_kdump: seek fail");
      return 1;
    }

    printf_notification("ycbcr_kdump 0x%llx+0x%llx",
                        (unsigned long long)DUMP_OFF,
                        (unsigned long long)DUMP_LEN);

    for (uint64_t off = DUMP_OFF; off < DUMP_OFF + DUMP_LEN; off += PAGE) {
      if (get_memory_dump(kbase + off, (uint64_t *)page, PAGE) < 0) {
        close(fd);
        munmap(page, PAGE);
        printf_notification("ycbcr_kdump stop @0x%llx",
                            (unsigned long long)off);
        return 1;
      }
      if (write(fd, page, PAGE) != (ssize_t)PAGE) {
        close(fd);
        munmap(page, PAGE);
        printf_notification("ycbcr_kdump: write fail");
        return 1;
      }
    }

    syscall(95, fd); /* fsync */
    close(fd);
    munmap(page, PAGE);
    printf_notification("ycbcr_kdump OK @0x%llx",
                        (unsigned long long)(DUMP_OFF + DUMP_LEN));
    return 0;
  }

#else
#error "MODE must be 0..3"
#endif
}
