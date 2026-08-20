#pragma once

/*
 * Offset relative to kernel_base of the Jcc to NOP.
 * Required for MODE=1. See docs/KERNEL_YCBCR_RE.md.
 */
/* jne EINVAL after pixel-fmt whitelist (full 9.00 dump). docs/KERNEL_YCBCR_RE.md */
#ifndef YCBCR_KPATCH_FIXED_OFF
#define YCBCR_KPATCH_FIXED_OFF  0x50ea26ull
#endif

#define KSCAN_START      0x100000ull
#define KSCAN_END        0xE00000ull
#define KSCAN_PAGE       0x1000u
#define PAIR_WINDOW      0x2000u

#define YCBCR_KPATCH_MAGIC_OK       0x59434231u
#define YCBCR_KPATCH_MAGIC_ALREADY  0x59434232u
#define YCBCR_KPATCH_MAGIC_FAIL     0x59434239u
#define YCBCR_KPATCH_MAGIC_FOUND    0x59434246u
