# SPIKE HEVC — videodec2 constants, error catalog, timing (Phase 1, Task 1.3)

Status: **ROUND 1 DONE (2026-08-19, FW 12.00) — ROUND 2 in flight**

Round 1 findings (console):
- Generic `sceVideodec2CreateDecoder` **rejects codecType=2** with
  `0x811d0204` = `ORBIS_VIDEODEC2_ERROR_CODEC_TYPE` (shadPS4
  `videodec_error.h`). resType 2/3 → `0x811d0203` RESOURCE_TYPE (resType=1
  is correct).
- **HEVC is a separate export: `sceVideodec2CreateHevcDecoder`** (OpenOrbis
  SDK header — shadPS4 doesn't implement it, hence it was invisible in
  emulator research). Round 2 routes HEVC through it; query keeps codec=1
  (QueryDecoderMemoryInfo itself validates codecType).
- Round-1 spike harness bug: decoder memories (cpu/gpu/cpuGpu) were never
  allocated → AVC CreateDecoder `0x811d0105` MEMORY_POINTER. Fixed in r2
  (alloc ONION/GARLIC/ONION like the real app).

## How to run

1. Install the Phase 1 `.pkg` (CATEGORY=gd) with GoldHEN active.
2. `moonlight.ini`: `videodec2_spike = true` → app prints the probe matrix and exits.
3. Capture the UDP log (`nc -ulk 9999`) — every row prints as
   `spike[LABEL]: resType=.. codec=.. prof=.. lvl=.. dpb=.. WxH => 0xERR cpu=.. gpu=.. cpuGpu=.. fb=..`
   followed by `CreateDecoder => 0xERR dec=0x..` when the query succeeded.
4. Then set `codec = hevc`, `videodec2_spike = false`, stream 1080p60 from
   vibepollo. Watch the decoder log lines (`orbis: HEVC stream ...`, `out fmt=0x..`)
   and per-frame decode ms in the stats block.

## Unknowns matrix (fill from the console run)

| # | Unknown | Probe | Result (query / CreateDecoder) | Accepted value |
|---|---------|-------|--------------------------------|----------------|
| 1 | resourceType for HEVC | res1 / res2 / res3 @ Main 1080p | | |
| 2 | codecType = 2 | res with codec=2 | | |
| 3 | profile = 200 (Main) | Main 1080p | | |
| 4 | profile = 202 (Main10) | Main10 1080p | | |
| 5 | maxLevel = 153 (L5.1) | 4K probes | | |
| 6 | maxDpbFrameCount = 6 | 4K probes | | |
| 7 | output frameFormat | `out fmt=0x..` log (SDR) | | |
| 8 | HEVC picture info struct | TBD (GetPictureInfo hex-dump) | | |
| 9 | bitstream acceptance | Sunshine HEVC Annex B as-is? | | |
| 10 | 1080p60 HEVC decode ms | stats decode=..ms | | |
| 11 | computeQueue required? | probe uses queue; stream without? | | |

## Error code catalog

| Config | Return | Meaning / next step |
|--------|--------|---------------------|
| resourceType=0 (known AVC) | 0x811D0203 | documented upstream |
| resourceType=1, codec=2 | | |
| resourceType=2, codec=2 | | |
| resourceType=3, codec=2 | | |
| codec=2, profile=200 | | |
| codec=2, profile=202 | | |
| lvl=153 @ 4K | | |
| dpb=6 @ 4K | | |

## Decode timing (1080p60 HEVC SDR)

- decode ms: __ (target ≤ AVC baseline +10%)
- convert ms: __ (YCbCr path ≈ 0)
- present ms: __

## Baseline reference (AVC, stock)

From Task 0.3 console baseline (fill in):
- decode: __ ms, convert: __ ms, present: __ ms @ 1080p60 H.264
