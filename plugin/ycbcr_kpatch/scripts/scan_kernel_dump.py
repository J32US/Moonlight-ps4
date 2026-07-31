#!/usr/bin/env python3
"""Offline RE helper: find ioctl 0xc0308206 / YCbCr check candidates in a PS4 kernel dump.

Usage:
  python3 scan_kernel_dump.py kernel.elf
  python3 scan_kernel_dump.py kernel.bin --raw --base 0
  python3 scan_kernel_dump.py kernel.elf --kaslr-base 0xffffffff80000000
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

IOCTL_IMM = bytes.fromhex("068230c0")  # 0xC0308206 LE
YCBCR_IMM = bytes.fromhex("00223208")  # 0x08322200 LE
BGRA_MASK = bytes.fromhex("81e1000000c081f900000080")  # and ecx,C0000000; cmp ecx,80000000
NOP6 = bytes([0x90] * 6)


def load_blob(path: Path, raw: bool) -> tuple[bytes, int]:
    data = path.read_bytes()
    if raw or data[:4] != b"\x7fELF":
        return data, 0
    # Minimal ELF64: use first PT_LOAD file offset as image base for file offsets
    if data[4] != 2 or data[5] != 1:
        return data, 0
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize = struct.unpack_from("<H", data, 54)[0]
    e_phnum = struct.unpack_from("<H", data, 56)[0]
    # Prefer executable PT_LOAD
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags = struct.unpack_from("<II", data, off)
        if p_type != 1:
            continue
        p_offset, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from("<QQQQQ", data, off + 8)
        if p_flags & 1:  # PF_X
            return data[p_offset : p_offset + p_filesz], p_vaddr
    return data, 0


def find_all(hay: bytes, needle: bytes) -> list[int]:
    out: list[int] = []
    start = 0
    while True:
        i = hay.find(needle, start)
        if i < 0:
            break
        out.append(i)
        start = i + 1
    return out


def is_jcc(b: bytes, at: int) -> tuple[int, str] | None:
    if at + 6 <= len(b) and b[at] == 0x0F and (b[at + 1] & 0xF0) == 0x80:
        rel = struct.unpack_from("<i", b, at + 2)[0]
        return 6, f"jcc32 rel={rel:+d} -> +{at + 6 + rel:#x}"
    if at + 2 <= len(b) and 0x70 <= b[at] <= 0x7F:
        rel = struct.unpack_from("<b", b, at + 1)[0]
        return 2, f"jcc8 rel={rel:+d} -> +{at + 2 + rel:#x}"
    return None


def hexdump(b: bytes, off: int, before: int = 16, after: int = 48) -> str:
    lo = max(0, off - before)
    hi = min(len(b), off + after)
    chunk = b[lo:hi]
    lines = []
    for i in range(0, len(chunk), 16):
        row = chunk[i : i + 16]
        hx = " ".join(f"{x:02x}" for x in row)
        mark = "<<" if lo + i <= off < lo + i + 16 else "  "
        lines.append(f"  {lo + i:08x}{mark} {hx}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dump", type=Path)
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=None,
                    help="Override image VA base for reported offsets")
    ap.add_argument("--window", type=lambda x: int(x, 0), default=0x2000)
    args = ap.parse_args()

    blob, vaddr = load_blob(args.dump, args.raw)
    base = args.base if args.base is not None else vaddr
    print(f"loaded {args.dump} size={len(blob)} image_base={base:#x}")

    ioctl_hits = find_all(blob, IOCTL_IMM)
    ycbcr_hits = find_all(blob, YCBCR_IMM)
    bgra_hits = find_all(blob, BGRA_MASK)
    print(f"P1 ioctl imm hits: {len(ioctl_hits)}")
    print(f"P2 YCbCr imm hits: {len(ycbcr_hits)}")
    print(f"P3 BGRA mask/cmp hits: {len(bgra_hits)}")

    candidates = []
    for io in ioctl_hits:
        near_y = [y for y in ycbcr_hits if abs(y - io) <= args.window]
        near_b = [b for b in bgra_hits if abs(b - io) <= args.window]
        if not near_y and not near_b:
            continue
        for site in near_y + near_b:
            # Jcc within 24 bytes after imm / pattern end
            scan_from = site + (12 if site in near_b else 4)
            for delta in range(0, 24):
                j = is_jcc(blob, scan_from + delta)
                if not j:
                    continue
                jlen, jdesc = j
                file_off = scan_from + delta
                koff = base + file_off
                kind = "YCbCr-imm" if site in near_y else "BGRA-mask"
                candidates.append((koff, file_off, jlen, kind, io, site, jdesc))
                break

    print(f"\n=== candidates ({len(candidates)}) ===")
    for koff, foff, jlen, kind, io, site, jdesc in candidates:
        print(f"\nPATCH_OFF={koff - base:#x}  abs={koff:#x}  kind={kind}  jlen={jlen}")
        print(f"  ioctl@{io:#x}  fmtpat@{site:#x}  {jdesc}")
        print(f"  pre-NOP:\n{hexdump(blob, foff)}")
        patched = bytearray(blob[foff : foff + jlen])
        print(f"  recommend: NOP {jlen}B -> {' '.join(f'{x:02x}' for x in NOP6[:jlen])}")

    if not candidates:
        print("\nNo paired candidates. Dump P1/P2/P3 neighborhoods for manual RE:")
        for label, hits, n in (("P1", ioctl_hits[:8], 4), ("P2", ycbcr_hits[:8], 4), ("P3", bgra_hits[:8], 12)):
            for h in hits:
                print(f"\n{label} @{h:#x} (k={base + h:#x}):\n{hexdump(blob, h, 16, 64)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
