# Baseline 1.5.0 — 1080p60 HEVC (verified live, 2026-08-20)

**Tag: `v1.5.0`** (commit `c99d7a8`). Branch `main` = this baseline, protected.

This is the known-good state to return to if the 4K exploration (branch
`4k-next`) breaks anything. Verified on console (FW 12.00 / GoldHEN):

| Metric | Value |
|---|---|
| Resolution / codec | 1080p60 HEVC (SDR) |
| decode | **1.2 ms** (depth=2 pipelined submit) |
| convert | **1.6 ms** (bounce 0.5 + bgra 1.0, SSE2) |
| present | 0.0 ms |
| fps / drop | 66–67 sustained / ≈0 |
| per-frame total | ≈2.8 ms (17 % of 16.7 ms budget) |

## The build

- `Moonlight-1.5.0.pkg` (and `Moonlight-1.1.0.pkg` copy) in
  `C:\Users\brenb\workspace\moonlight-ps4-build\`
- Console INI: 1920×1080 @ 60 fps, 45 Mbps, `codec = hevc`, `hdr = false`,
  `dec_pipeline_depth = 2`, `dec_au_onion = true`, `bgra_workers = 6`,
  `slices_per_frame = 2`, `dec_thread_prio = 700`
- Contents (all console-verified): HEVC HW decode via the Apple TV cell
  (resType=1 + codecType=0xee049, prof 1, lvl 120, dpb 4), depth-2 pipeline,
  ONION AUs, SSE2 NV12→BGRA convert (AVX1 float kernel reverted — slower on
  Jaguar), 6 convert workers, BGRA LINEAR present at 1080p.

## What is NOT in this baseline (tabled / experimental)

All of the following exists in git history and on branch `4k-next`, but is
**inert at 1080p** (gated on `w >= 3840`):

- `hdr = true` INI key + Main10 negotiation (profile 2 decoder works;
  HDR10 present register `0x88740000` still rejects at 4K with
  `0x80290003`)
- 4K TILED present (register + flip work; DCE tile swizzle still garbled —
  GCN DISPLAY 32bpp microtile implemented, round-trips host-side, but the
  console image is not right yet)
- AVX1 / HDR10 convert kernels (`hdr10_convert.c`) — unused by the SSE2
  SDR path
- FW 12.00 kernel RE (ioctl `0xc0308206` dispatcher @ `0x51b000`, format
  whitelist @ `0x51b856`) — 12.00 already whitelists BGRA + HDR10; the
  YCbCr kpayload project from 9.00 is obsolete

## Resuming 4K

```bash
git checkout 4k-next   # starts at v1.5.0 + 4K work-in-progress
```

See `docs/SPIKE_HEVC.md` and the `ps4-homebrew` skill
(`references/videodec2-hevc-spike.md`) for the full RE trail.
