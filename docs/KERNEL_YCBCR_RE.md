# RE: VideoOut YCbCr ioctl kernel reject (FW 9.00)

## Userspace evidence (plugin 1.45–1.46)

| Case | `ioctl 0xc0308206` | Layout | Result |
|------|--------------------|--------|--------|
| BGRA `0x80000000` | `ret=0` | `a5a5` / `+0x08=2` / `addr[last]@+0x10` | OK |
| YCbCr `0x08322200` | `ret=-1` → `0x80290001` | **same** layout and same VAs | FAIL |
| `RegisterBufferAttribute` YCbCr | — | — | `rc=0` |
| `AddBuffer[0]` / `RegisterBuffers` n=1 | — | — | `0x80290001` |

`libSceVideoOut` validate patches are already open; the reject is **after** the syscall.

> Note: `0x80290001` is `ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE`, not `INVALID_ADDRESS` (`0x80290002`). Userspace maps the ioctl failure (`js` after `call ioctl`) to that code.

## Ioctl encoding

```
0xc0308206 = IOC_INOUT (0xc0000000)
           | (0x30 << 16)   // size 0x30
           | (0x82 << 8)    // group
           | 0x06           // nr
```

Group `0x82` is **not** `gbase` (wiki: group `0x45`). Likely device: `/dev/dce` / VideoOut-GC (fd≈21 in diag).

## Full dump (KernelDumper VV1LD)

File: local dump at `pkg/assets/misc/kernel_900.bin` (gitignored — **not
tracked, not shipped in the `.pkg`**; produce with `plugin/kernel_dumper_900`).

| Field | Value |
|-------|-------|
| Size | `0xd00000` (13 MiB) |
| Format | ELF64 FreeBSD |
| Image base (KASLR, this dump) | `0xffffffff882e0000` |
| Text `filesz` | `≈0xcfe0b8` |
| Coverage | **complete** (PT_LOAD XR) |

Patch offsets are **relative to `kernel_base`** (= file offset in this dump).

### Patterns

| ID | Bytes (LE) | Hits |
|----|------------|------|
| P1 `06 82 30 c0` | `0xc0308206` | **2** @ file `0x50d4d8`, `0x50d4fa` |
| P2 `00 22 32 08` | YCbCr imm | **0** (format does not appear as a literal) |
| P3 BGRA mask/cmp | — | 1 @ `0xdc418` (unmatched; no patch) |

### Ioctl handler @ `kbase+0x50d480`

Dispatches cmds `8206` / `8203` / `8207` / `8204`. Common error: `mov r14d, 0x16` (EINVAL) @ `0x50dbb4`. Path 8206 uses magic `xor …, 0xa5a5` (matches diag).

### Pixel-format whitelist @ `0x50e6c9`–`0x50ea2c`

Tree of `cmp eax, imm` / `je` → success `0x50ea2c`; failures → `0x50dbb4` (EINVAL).

- BGRA `0x80000000` → success (`cmp` @ `0x50e70c`).
- **`0x08322200` (YCbCr) is not in the list.**
- Signed simulation: YCbCr falls into `@0x50ea1a`:
  - `cmp $0xc1760000` / `je` success
  - `cmp $0xc1060000`
  - **`jne 0x50dbb4` @ `0x50ea26`** (`0f 85 88 f1 ff ff`) → EINVAL

`RegisterBufferAttribute` YCbCr already returns `rc=0` in userspace; the RegisterBuffers “sameVA” reject matches this format re-validation in the 8206 handler.

## Stable patch offset

| Field | Value |
|-------|-------|
| `kernel_base` | `rdmsr(LSTAR) - 0x1C0` (FW 9.00) |
| `PATCH_OFF` | **`0x50ea26`** |
| Prolog (pre-NOP) | `0f 85 88 f1 ff ff` (`jne` → EINVAL) |
| Epilog / idempotence | `90 90 90 90 90 90` → do not rewrite |
| Narrower alternative | Imm @ `0x50ea22`: `00 00 06 c1` → `00 22 32 08` (`cmp` accepts YCbCr; loses `0xc1060000`) |

Preferred MODE=1 patch: **NOP 6 B of the `jne`** (matches kpayload `is_jcc`). Accepts any format that reaches bucket `@0x50ea1a` (incl. YCbCr).

```bash
# Build apply (do not send to BinLoader without permission):
make -C plugin/ycbcr_kpatch \
  EXTRA_CFLAGS='-DMODE=1 -DYCBCR_KPATCH_FIXED_OFF=0x50ea26'
```

## Offline tool

```bash
python3 plugin/ycbcr_kpatch/scripts/scan_kernel_dump.py \
  /path/to/kernel_900.bin --raw --base 0
```

(Use a local dump; do not commit `kernel_*.bin` into the repo.)

## Useful public symbols (9.00)

| Symbol / define | Offset / note |
|-----------------|---------------|
| `XFAST_SYSCALL` | `0x1C0` |
| `PRISON0` | `0x0111F870` |
| `ROOTVNODE` | `0x021EFF20` |
| `COPYOUT` | `0x002715B0` |

## Post-apply (console 2026-07-29)

Klog: `ycbcr_kpatch OK 0x50ea26`. `0x80290001` **no longer** appears.

With BGRA baseline → Unregister → YCbCr (app 0.7.28 era):

| Code | Name | Note |
|------|------|------|
| `0x80290010` | `SLOT_OCCUPIED` | slots not fully released |
| `0x80290009` | `RESOURCE_BUSY` | attribute still in use |
| `ioctl_called=0` | — | failed in userspace before the ioctl |

Plugin **1.48** + app **0.7.30** era: YCbCr with `n=1` (only ioctl path that worked).
`n=3` still `0x80290001` — another kernel check pending RE.

Current packaging target: app **1.0.0** (`Moonlight-1.0.0.pkg`), plugin **1.43+**,
`CATEGORY=gd`. Defaults: `prefer_hw=true`, `prefer_ycbcr=false` (BGRA usable path;
enable YCbCr via Settings / `moonlight.ini` after kpayload + plugin).

## Conclusion

- YCbCr format reject = kernel whitelist @ handler `0xc0308206`.
- `YCBCR_KPATCH_FIXED_OFF = 0x50ea26` (NOP `jne` → EINVAL) — confirmed in klog.
- After kpatch, mind slot lifecycle (no prior BGRA).
- Success criteria: kpayload + plugin + Moonlight → `ioctl ret=0` and `present_mode=YCbCr`.

## SubmitFlip YCbCr (post-register)

With kpatch + plugin 1.48 (`FIX_MODE=2` forces ioctl `+0x08=2`):

| Step | Result |
|------|--------|
| `RegisterBuffers` n=1 | `ioctl ret=0` |
| FB content (grey) | OK (`y0=a0a0`) |
| `SubmitFlip` | **`0x80290001`**, `cur=-1` |
| BGRA `SubmitFlip` | `rc=0` (image OK; convert≈1.3 s) |

No second format whitelist in flip ioctls `8203`/`8207`. Hypothesis: tag
`+0x08=2` (BGRA) stored in kernel (`+0x20a0`) clashes with YCbCr attr; also
`priv` lacked `BIT_YCC_EXT (0x20)`.

Plugin **1.49**: `FIX_MODE=0` (natural tag), `priv\|=0x20`, mark slots `+0x150`,
hook `SubmitFlip` (re-flags + klog).

### “4 tiles” + green image (1.50→1.52)

Symptom after flip OK: 4 horizontal copies + green tint (bpp×4 in
`validate+0x813` `lea rdi,[rcx*4]`).

- **1.51** `mov rdi,rcx` during register → **crash** (reverted in 1.52).
- **0.7.38–0.7.39**: blit×4 / hrep4 — centered image (only OK geometry).
- **1.53–1.54 / 0.7.40–41**: bpp1 8B + permanent + attr480 → register OK or
  `0x80290005`, but **still 4 tiles**. The `lea` does not program DCE stride.
- **0.7.42**: back to **hrep4** (proven geometry); ioctl size `0x30` only
  carries pitch@+0x2c — DCE dims come from another path.
- Vehicle: `plugin/ycbcr_kpatch/` MODE=1 (idempotent).
