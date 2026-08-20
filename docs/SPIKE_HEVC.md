# SPIKE HEVC — videodec2 constants, error catalog, timing (Phase 1, Task 1.3)

Status: **COMPLETE — all unknowns answered on console (FW 12.00, 2026-08-20)**

Final validated matrix:
- **HEVC create path: `sceVideodec2CreateHevcDecoder`** (dedicated export; the
  generic `CreateDecoder` rejects HEVC entirely). Also on 12.00:
  `sceVideodec2QueryHevcDecoderMemoryInfo`, `sceVideodec2GetHevcPictureInfo`.
- **Struct values are IDENTICAL to AVC**: codecType=1, profile=100, level=51,
  resourceType=1. The decoder type comes from the entry point, NOT the fields.
  Guessed 200/202/153 all → 0x811d0205 PROFILE_LEVEL.
- Query MUST match create's maxDpbFrameCount: 4K row with query dpb=4 vs
  create dpb=6 → 0x811d0104 MEMORY_SIZE.
- codecType=2 anywhere → 0x811d0204 CODEC_TYPE. resType 2/3 → 0x811d0203.
- Open: Main10 profile value (Phase 3 spike: try 102/202), output frameFormat,
  bitstream acceptance, decode timing — the live stream test.

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

## ✅ FINAL RESULT — HEVC DECODES ON FW 12.00 (2026-08-20)

**The wall is broken.** Rounds 1-22 (universal 0x811d0303 → 0x811d0200) ended
by RE of retail apps. The create config that works (Apple TV CUSA24386's
combo, verified live):

| field | value |
|---|---|
| resourceType | **1 (EMBEDDED)** — NOT 0xb6c8 (Netflix's; gate rejects type 4) |
| codecType | **0xee049** (packed 0xee04900000001 at config+0x08) |
| profile | 1 (Main) / 2 (Main10) — both decode |
| maxLevel | 120 (1080p) / 153 (4K); 123 OK; 0 REJECTED (0205) |
| maxDpbFrameCount | 4 (1080p) / 6 (4K) — must match query |
| decodePipelineDepth | 1 (0 worked too) |
| cpuThreadPriority | -1 |
| cpuAffinityMask | 0 |
| checkMemoryType | 0 |
| entry point | generic QueryDecoderMemoryInfo + CreateDecoder |
| computeQueue | required (0x811d0110 without); pipe 0/0 fine |

Full history of the 22 rounds, the type-2-vs-type-4 gate discovery, and the
Netflix/Apple TV RE recipes: `references/videodec2-hevc-spike.md` in the
ps4-homebrew skill (kept in sync).

## Decode timing (1080p60 HEVC SDR) — LIVE VERIFIED

- decode: **5.4 ms** (H.264 baseline 6.3 ms — HEVC is FASTER)
- convert: **1.7 ms** (BGRA bounce 0.5 + bgra 1.2)
- present: **0.0 ms**
- drops: **0 sustained** @ 1080p60, 50 Mbps, Sunshine 7.1 / vibepollo

## Baseline reference (AVC, stock)

From Task 0.3 console baseline:
- decode: **6.3 ms**, convert: **1.6 ms**, present: **0.1 ms** @ 1080p60 H.264
