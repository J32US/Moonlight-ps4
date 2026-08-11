# Console validation — YCbCr + convert &lt;5 ms (1080p60)

Strict order. Do not skip steps.

## 0. Usable path today (BGRA)

Without kpayload, use BGRA presentation (visible image; slow NV12→BGRA convert):

```ini
prefer_hw = true
prefer_ycbcr = false
```

These are the app defaults (`moonlight.ini` / Settings menu). Edit host and options
on-console via the UI menu, or in `/data/moonlight/moonlight.ini` after install.

OK criteria: `present_mode=BGRA`, `RegisterBuffers => 0`, first AU `NAL types=[7,8,…]`
(head `…0167…`, **not** `…010067…`), no `Decode 0x811d0303`, frames on screen.

## 1. Packaging and install (YCbCr)

1. Build app: `cmake --build build-ps4` → `Moonlight-1.1.0.pkg`
2. Confirm `CATEGORY=gd` in the `.pkg` (GoldHEN does **not** inject plugins with `gde`)
3. Install `plugin/bin/ycbcr_unlock.prx` → `/data/GoldHEN/plugins/`
4. `/data/GoldHEN/plugins.ini`:

```ini
[MLNT00001]
/data/GoldHEN/plugins/ycbcr_unlock.prx
```

5. GoldHEN → Enable plugins
6. Install the `.pkg` and set `moonlight.ini` / Settings (`prefer_hw=true`,
   `prefer_ycbcr=true`, 1920×1080@60)
7. Build kpayload (host): `export PS4SDK=…/ps4-payload-sdk && make -C plugin/ycbcr_kpatch`
   → `plugin/bin/ycbcr_kpatch_900.bin` (FW **9.00** only)

## 1b. Kernel kpayload (required for YCbCr RegisterBuffers)

Userspace alone is not enough: ioctl `0xc0308206` still returns `ret=-1` → `0x80290001`
(`INVALID_VALUE`) with the same layout as BGRA OK. RE and patterns:
[`KERNEL_YCBCR_RE.md`](KERNEL_YCBCR_RE.md).

**Boot order (every reboot):**

1. Boot console → GoldHEN (jailbreak) active, BinLoader on TCP **9090**
2. Send the kpayload **once**:

```bash
nc -w 3 <PS4_IP> 9090 < plugin/bin/ycbcr_kpatch_900.bin
```

3. Expected notification:
   - **Ping (default):** `ycbcr_kpatch ping OK` — does not touch the kernel. If this
     panics, the problem is not the YCbCr patch (BinLoader/format).
   - With `FIXED_OFF` + MODE=1: `ycbcr_kpatch OK 0x…`
4. Then open Moonlight (loads `ycbcr_unlock.prx`)

| Kpayload signal | Meaning |
|-----------------|---------|
| `ping OK` | BinLoader OK; no kernel patch yet |
| `OK 0x…` | NOP applied (MODE=1) |
| `already 0x…` | Already patched |
| trap 12 | Do not resend old bins; reboot and use ping (~5.5 KB) |

**Risk:** wrong offset → **kernel panic**. The patch does not persist across reboot.
FW 9.00 only. Automatic scan is disabled (it caused trap 12).

## 1. Plugin OK

On Moonlight launch:

| Signal | Expected |
|--------|----------|
| Notification / klog | `ycbcr 1.63` |
| Marker | `/data/moonlight/ycbcr_unlock.loaded` exists |
| UDP log / `debug.log` | `plugin ycbcr_unlock: marker OK` |
| Diag (optional) | `/data/moonlight/vo_diag.txt` with `ver=0x0000013f` (plugin 1.63) |

If the marker is missing: check `plugins.ini`, section `[MLNT00001]`, `CATEGORY=gd`.

## 1b. Ioctl payload (diagnosing 0x80290001)

Error `0x80290001` (`INVALID_VALUE`; often misquoted as “INVALID_ADDRESS”)
on `RegisterBuffers` can be partial struct fill **or** a **kernel** reject after a
well-formed ioctl. Bisect evidence M1–M5 + ref (1.39) and spy 1.46: with
BGRA-identical layout (`a5a5` / `+0x08=2` / VA) YCbCr still gets `ioctl ret=-1`;
the final check is not userspace-only.

```
attr: fmt=0x08322200 tile=1 1920x1080 pitch=1920 n=3 bisect=5
  hook_addr[0]=0x201200000
  hook_addr[1]=0x201600000
  hook_addr[2]=0x201a00000
ioctl: called=1 filled=1 ... ret=0xffffffff
  ioctl_fields: +0x00=0xa5a5 +0x10=0x201200000 +0x18=0x0 +0x20=0x0
```

Actual payload layout:

| Offset | Observed content | Action |
|--------|------------------|--------|
| `+0x00` | `0xa5a5` (magic/set) | **Do not touch** |
| `+0x08` | `0` on YCbCr / `2` on BGRA OK | **Force `2`** (1.43) |
| `+0x10` | `addr[0]` | OK |
| `+0x18` | `0` (missing `addr[1]`) | Fill |
| `+0x20` | `0` (missing `addr[2]`) | Fill |
| `+0x2c` | pitch (`0x780`=1920) | OK |

Plugin **1.43+** (`FIX_MODE=2` era): fills ptrs / tag `+0x08`. With correct ptrs and
same VA as BGRA, if still `ret=-1` → need the **kpayload** (step 0b). Later builds
(1.49+) default to natural tag (`FIX_MODE=0`) once kpatch is applied.

| Signal | Diagnosis |
|--------|-----------|
| `fixup_done=1` and `+0x08=2` and `+0x18/+0x20 == hook_addr[1/2]` | Userspace fixup OK |
| `ioctl ret=0` / `rc=0x00000000` | Register OK (userspace + kernel) |
| `ioctl ret=-1` with BGRA-identical layout | Kernel: apply `ycbcr_kpatch_900.bin` |
| `AddBuffer[0] => 0x80290001` | Path B discarded (seen in fix1) |

## 1c. Patch bisect (console result)

| Build | Patches | `rc` 1080 | Conclusion |
|-------|---------|-----------|------------|
| M1–M5 / ref | gate → full | `0x80290001` | **No patch causes the partial fill** |
| fix1 (AddBuffer) | full + B | attr OK, AddBuffer=`01` | Path B not enough |
| fix2 (1.39) | bad fixup | `fixup_done=0` | Old condition (required `filled=0`) |

Build variants:

```bash
make -C plugin/ycbcr_unlock bisect   # m1..m5 with FIX_MODE=0
make -C plugin/ycbcr_unlock          # production (current default FIX_MODE=0; 1.43+ lineage)
make -C plugin/ycbcr_unlock FIX=1    # AddBuffer
make -C plugin/ycbcr_unlock FIX=2    # explicit fixup
```

## 1d. Active fix (userspace + kernel)

1. Kpayload sent (step 0b) → notification `ycbcr_kpatch OK`
2. PRX (1.43+): `vo_diag` with fixup / natural tag as built
3. Coherent `ioctl_fields`; **`ioctl ret=0`** on the YCbCr attempt
4. `present: YCbCr try ... => 0x00000000` and `present_mode=YCbCr`

If ioctl stays `-1` after kpayload OK: check offset in the notification,
rescan dump (`plugin/ycbcr_kpatch/scripts/scan_kernel_dump.py`) and set
`YCBCR_KPATCH_FIXED_OFF`. If the pattern is missing: possible check in the IPMI
process (Phase 3 of the plan) — do not poke blindly (panic risk).

## 2. YCbCr registered (no BGRA)

On the first stream:

```
present: YccPrivilege=... SysUpdatePrivilege=...
present: YCbCr try tile=1 pitch=1920 1920x1080 => 0x00000000
present: YCbCr420_BT709 OK ...
present_mode=YCbCr plugin=OK buf_h=1080 blit=...
```

**Fail:** any `FATAL YCbCr` or `present_mode=BGRA` with `prefer_ycbcr=true` → stop; do not measure latency.

## 3. Stats 1080p60 (Phase A — memcpy)

Phase A criteria (plugin + correct 1088→1080 copy):

| Metric | Target |
|--------|--------|
| `decode` | ≈ 1–2 ms |
| `convert` | ≈ 30–50 ms if `blit=cpu_wc`; **never** ~600 ms |
| `present` | ≈ 1–2 ms |
| Image | correct (no shifted UV / green tint) |

If `convert≈600 ms` → you are on BGRA; return to steps 1–2.

## 4. GPU / fast blit stats (Phase B)

With `blit=wb_garlic` or `onion_alias`:

| Metric | Target |
|--------|--------|
| `convert` | **&lt; 5 ms** sustained |
| `decode + convert + present` | **&lt; ~12 ms** average |
| Limelight overflow | not sustained |

Useful boot log:

```
orbis: framebuf type=10 ...    # WB_GARLIC
nv12_blit: decoder FB WB_GARLIC
# or
nv12_blit: Onion alias OK ...
present_mode=YCbCr plugin=OK buf_h=1080 blit=wb_garlic
```

## 5. Quick checklist

- [ ] After every reboot: `ycbcr_kpatch_900.bin` → BinLoader `:9090` → `OK`/`already`
- [ ] Plugin marker + notification `ycbcr 1.63`
- [ ] `vo_diag`: `ioctl ret=0` on YCbCr RegisterBuffers
- [ ] `present: YCbCr try … => 0x00000000` and `present_mode=YCbCr plugin=OK`
- [ ] No `BGRA fallback` / no convert ~600 ms
- [ ] Correct UV (1088→1080)
- [ ] `convert < 5 ms` with cacheable blit
- [ ] Playable 1080p60 without sustained overflow
- [ ] Exit to menu without `SUBMITDONE_TIMEOUT` (queue cleanup OK)
