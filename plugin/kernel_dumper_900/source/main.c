/*
 * kernel_dumper_900 — VV1LD KernelDumper port for FW 9.00 / GoldHEN.
 * Do not send to console until ping is validated; blind copyout can trap 12.
 */

#include "ps4.h"
#include "defines.h"

static void notify(char *message) {
  char buffer[512];
  sprintf(buffer, "%s\n\n\n\n\n\n\n", message);
  sceSysUtilSendSystemNotificationWithText(0x81, buffer);
}

static void page_zero(void *uaddr, size_t n) {
  uint8_t *p = (uint8_t *)uaddr;
  for (size_t i = 0; i < n; i++)
    p[i] = 0;
}

/* copyout of 1 page. If ret -1, zero userspace (like VV1LD + bzero). */
static int kdump(struct thread *td, struct kdump_args *args) {
  (void)td;
  uint8_t *kernel_base =
      &((uint8_t *)__readmsr(0xC0000082))[-KERN_BASE_PTR];
  int (*copyout)(const void *kaddr, void *uaddr, size_t len) =
      (void *)(kernel_base + KERN_COPYOUT);

  uint64_t kaddr = args->payload_info_dumper->kaddr;
  uint64_t uaddr = args->payload_info_dumper->uaddr;

  int cpRet = copyout((const void *)kaddr, (void *)uaddr, PAGE_SIZE);
  if (cpRet == -1) {
    page_zero((void *)uaddr, PAGE_SIZE);
    return cpRet;
  }
  return cpRet;
}

static int kpayload(struct thread *td, struct kpayload_args *args) {
  struct filedesc *fd = td->td_proc->p_fd;
  struct ucred *cred = td->td_proc->p_ucred;

  uint8_t *kernel_base =
      &((uint8_t *)__readmsr(0xC0000082))[-KERN_BASE_PTR];
  void **got_prison0 = (void **)&kernel_base[KERN_PRISON0];
  void **got_rootvnode = (void **)&kernel_base[KERN_ROOTVNODE];
  int (*copyout)(const void *kaddr, void *uaddr, size_t len) =
      (void *)(kernel_base + KERN_COPYOUT);

  cred->cr_uid = 0;
  cred->cr_ruid = 0;
  cred->cr_rgid = 0;
  cred->cr_groups[0] = 0;
  cred->cr_prison = *got_prison0;
  fd->fd_rdir = fd->fd_jdir = *got_rootvnode;

  /* Sony privs (same td_ucred offset as VV1LD / classic exploits). */
  void *td_ucred = *(void **)(((char *)td) + 304);
  *(uint64_t *)(((char *)td_ucred) + 96) = 0xffffffffffffffffull;
  *(uint64_t *)(((char *)td_ucred) + 88) = 0x3801000000000013ull;
  *(uint64_t *)(((char *)td_ucred) + 104) = 0xffffffffffffffffull;

  uint64_t uaddr = args->payload_info->uaddr;
  void *kbase = kernel_base;
  copyout(&kbase, (void *)uaddr, 8);
  return 0;
}

int _main(struct thread *td) {
  (void)td;
  initKernel();
  initLibc();
  initNetwork();
  initPthread();

#ifdef DEBUG_SOCKET
  struct sockaddr_in server;
  server.sin_len = sizeof(server);
  server.sin_family = AF_INET;
  server.sin_addr.s_addr = DUMP_IP;
  server.sin_port = sceNetHtons(DUMP_PORT);
  memset(server.sin_zero, 0, sizeof(server.sin_zero));
  int sock = sceNetSocket("kdump", AF_INET, SOCK_STREAM, 0);
  sceNetConnect(sock, (struct sockaddr *)&server, sizeof(server));
  int flag = 1;
  sceNetSetsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
#endif

  uint64_t *dump =
      mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE,
           -1, 0);
  if (!dump || dump == MAP_FAILED) {
    return 1;
  }
  memset(dump, 0, PAGE_SIZE);
  mlock(dump, PAGE_SIZE);

  struct payload_info payload_info;
  payload_info.uaddr = (uint64_t)dump;
  kexec(&kpayload, &payload_info);

  initSysUtil();
  notify("kdump900: patched");

  uint64_t kbase = 0;
  memcpy(&kbase, dump, 8);
  if (!kbase) {
    notify("kdump900: no kbase");
    munmap(dump, PAGE_SIZE);
    return 1;
  }

#ifndef DEBUG_SOCKET
  mkdir("/data/moonlight", 0777);
  int fd = open(KERN_FILEPATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    notify("kdump900: open fail");
    munmap(dump, PAGE_SIZE);
    return 1;
  }
#endif

  notify("kdump900: dumping…");

  struct payload_info_dumper di;
  uint64_t pos = 0;
  for (unsigned i = 0; i < KERN_DUMPITER; i++) {
    di.kaddr = kbase + pos;
    di.uaddr = (uint64_t)dump;
    kexec(&kdump, &di);

#ifdef DEBUG_SOCKET
    sceNetSend(sock, dump, PAGE_SIZE, 0);
#else
    if (write(fd, dump, PAGE_SIZE) != (ssize_t)PAGE_SIZE) {
      close(fd);
      munmap(dump, PAGE_SIZE);
      notify("kdump900: write fail");
      return 1;
    }
#endif
    pos += PAGE_SIZE;

    /* Progress every 1 MiB */
    if ((pos & 0xfffffull) == 0) {
      char msg[64];
      sprintf(msg, "kdump900: %u MiB", (unsigned)(pos >> 20));
      notify(msg);
#ifndef DEBUG_SOCKET
      syscall(95, fd); /* fsync */
#endif
    }
  }

#ifdef DEBUG_SOCKET
  sceNetSocketClose(sock);
  notify("kdump900: socket OK");
#else
  syscall(95, fd);
  close(fd);
  notify("kdump900: file OK");
#endif

  munmap(dump, PAGE_SIZE);
  return 0;
}
