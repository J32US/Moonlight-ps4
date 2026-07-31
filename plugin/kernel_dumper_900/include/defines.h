/*
 * Port of VV1LD/PS4-KernelDumper → FW 9.00 + GoldHEN BinLoader (:9090).
 * Based on https://github.com/VV1LD/PS4-KernelDumper (4.05/4.55/5.05).
 *
 * Same technique: kexec + page-by-page copyout (Shadow).
 * On 9.00, copyout of an unmapped page usually trap 12 (not -1); the port
 * is ready to try, no guarantees.
 */

#pragma once

#define KERN_VER 900

/* Comment out to dump to file; uncomment = socket to PC (see README). */
/* #define DEBUG_SOCKET */

#define KERN_BASE_PTR   0x000001C0ull /* K900_XFAST_SYSCALL */
#define KERN_COPYOUT    0x002715B0ull /* K900_COPYOUT */
#define KERN_PRISON0    0x0111F870ull /* K900_PRISON_0 */
#define KERN_ROOTVNODE  0x021EFF20ull /* K900_ROOTVNODE */
/* kernel printf/bzero not needed: userspace log + inline zero */

/* Text PT_LOAD 9.00 ≈ 0xcfe0b8; small margin. Override: -DKERN_DUMPSIZE=… */
#ifndef KERN_DUMPSIZE
#define KERN_DUMPSIZE   0x00D00000u /* 13 MiB */
#endif

/* PAGE_SIZE = 16 KiB comes from libPS4 memory.h */
#define KERN_DUMPITER   (KERN_DUMPSIZE / PAGE_SIZE)

#define KERN_FILEPATH   "/data/moonlight/kernel_900.bin"

#define IP(a, b, c, d) (((a) << 0) + ((b) << 8) + ((c) << 16) + ((d) << 24))

#ifdef DEBUG_SOCKET
#ifndef DUMP_IP
#define DUMP_IP         IP(192, 168, 0, 91)
#endif
#ifndef DUMP_PORT
#define DUMP_PORT       9023
#endif
#endif

struct payload_info {
  uint64_t uaddr;
};

struct payload_info_dumper {
  uint64_t uaddr;
  uint64_t kaddr;
};

struct kdump_args {
  void *syscall_handler;
  struct payload_info_dumper *payload_info_dumper;
};

struct kpayload_args {
  void *syscall_handler;
  struct payload_info *payload_info;
};
