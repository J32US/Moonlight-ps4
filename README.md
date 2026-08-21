# moonlight-ps4

Unofficial [Moonlight](https://moonlight-stream.org/) port (Sunshine/GameStream
game-streaming client) for jailbroken PS4 (GoldHEN), built on the
[OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) toolchain.

> ## 🙏 Big credit to the original author
>
> This project is a fork of **moonlight-ps4** by
> [**Jaime Jimenez**](https://github.com/JaimeJimenezG) —
> [JaimeJimenezG/Moonlight-ps4](https://github.com/JaimeJimenezG/Moonlight-ps4) —
> the original Moonlight port for jailbroken PS4. Without his work
> (pairing, streaming, UI, packaging — the whole base this fork builds on),
> none of the HEVC/4K work here would exist. All credit for the original
> project goes to him; this fork only adds on top of it.

Full port plan: [PLAN.md](PLAN.md).

**Current version: 1.5.0 "Eclipse"** — 1080p60 HEVC hardware decode baseline
(verified live on PS4 Pro, FW 12.00). See [docs/BASELINE_1.5.0.md](docs/BASELINE_1.5.0.md).

## Status

Playable client for jailbroken PS4 (GoldHEN / FW 12.00 validated):

- [x] Pairing, applist, launch, and H.264 streaming against Sunshine
- [x] **HEVC (H.265) hardware decode** via `libSceVideodec2` — 1080p60,
      decode 1.2 ms/frame (faster than the H.264 path), ≈0 drops
- [x] Audio (Opus + `sceAudioOut`)
- [x] Software video (FFmpeg) and hardware decode (`libSceVideodec2`, `prefer_hw`)
- [x] DualShock 4 input (`scePad`)
- [x] On-console UI (APPS / SETTINGS + on-screen keyboard) and INI config
- [x] Installable `.pkg` (`CATEGORY=gd`)
- [x] Optional performance overlay and Videodec2 / BGRA tuning knobs

### What's new in 1.5.0

- **HEVC hardware decode** — Apple TV-style create config (resourceType=1,
  codecType=0xee049, profile 1/2, level 120, dpb 4), verified live:
  decode **1.2 ms**, convert **1.6 ms**, present 0.0 ms, 66–67 fps
  sustained @ 1080p60 (≈2.8 ms/frame, 17 % of the 16.7 ms budget)
- **Depth-2 decode pipeline** (`dec_pipeline_depth = 2`) with ONION
  access-unit buffers (`dec_au_onion = true`)
- **SSE2 NV12→BGRA convert** — the AVX1 float kernel was reverted (3.75×
  slower on Jaguar); SSE2 + 6 workers is the fast path
- **Tuned fresh-install defaults**: `codec = hevc`, 45 Mbps, 6 bgra workers,
  2 slices/frame, decoder thread priority 700

### Experimental (not in the 1.5.0 baseline)

Gated on `w >= 3840`; present in git history / on branch `4k-next`, but inert
at 1080p:

- `hdr = true` INI key + Main10 negotiation (profile 2 decoder works; HDR10
  present register still rejects at 4K with `0x80290003`)
- 4K TILED present (register + flip work; DCE tile swizzle still garbled)
- AVX1/HDR10 convert kernels (`hdr10_convert.c`) — unused by the SSE2 SDR path

### TODO

- [ ] 4K TILED present — fix DCE tile swizzle (top ~128–200 px garble on `kTileModeDisplay_2dThin` frames; test-pattern diag pending)
- [ ] HDR10 at 4K — HDR10 present register rejects with `0x80290003`

Full port plan and longer backlog: [PLAN.md](PLAN.md). Validation notes:
[docs/CONSOLE_VALIDATE.md](docs/CONSOLE_VALIDATE.md). HEVC reverse-engineering
history: [docs/SPIKE_HEVC.md](docs/SPIKE_HEVC.md).

## Requirements (development)

- Linux x86_64 with `clang`, `ld.lld`, `cmake`, `ninja`, `git`, and `curl`
- OpenOrbis toolchain v0.5.4 (default: `~/ps4dev/OpenOrbis/PS4Toolchain`)
- Cross-built FFmpeg for PS4 (H.264 decoder only): `scripts/build_ffmpeg_ps4.sh`
- For `PkgTool.Core`: `libssl.so.1.1` / `libcrypto.so.1.1` in `~/ps4dev/hostlibs/usr/lib`
- PS4 with GoldHEN-compatible firmware (plan validated on **12.00**; the
  YCbCr kpayload path is 9.00)
- Host PC running [Sunshine](https://github.com/LizardByte/Sunshine)



## Dependencies (third_party)

Vendored libraries live as **git submodules** pinned in [`third_party/DEPS`](third_party/DEPS).
Git only stores the commit SHA for each submodule — not a full copy of upstream.
Orbis-specific fixes are **not** committed inside those repos; they live under
[`patches/`](patches/) and are applied after checkout.

```bash
git clone <this-repo>
scripts/setup_deps.sh    # checkout pins from third_party/DEPS + apply patches/
```

Equivalent manual flow: `git submodule update --init --recursive` then
`scripts/apply_patches.sh`. After patches, `third_party/*` working trees look
dirty — that is expected; commit patch files, not submodule dirt.

To bump a dependency on purpose: edit the pin in `third_party/DEPS`, update the
submodule to that commit, refresh patches if needed (`scripts/refresh_patches.sh`),
and commit the new gitlink + DEPS (+ patches).

## Build

```bash
git clone <this-repo>
cd moonlight-ps4
scripts/setup_deps.sh                # pinned third_party + Orbis patches
scripts/build_ffmpeg_ps4.sh          # once (→ ~/ps4dev/ffmpeg-ps4)

# PS4 app + .pkg (sources env.sh, configures, builds, packages)
scripts/build_pkg.sh
# scripts/build_pkg.sh --clean       # wipe build-ps4/ and reconfigure

# Optional: Linux development CLI (pairing / H.264 dump)
cmake -B build-host -G Ninja && cmake --build build-host
```

Installable artifact: `build-ps4/Moonlight-1.5.0.pkg`
(also `IV0000-MLNT00001_00-MOONLIGHTPS40000.pkg`).

`build_pkg.sh` runs `source scripts/env.sh` itself. Manual CMake equivalent:

```bash
source scripts/env.sh
cmake -B build-ps4 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/openorbis.cmake
cmake --build build-ps4
```

## Usage on PS4

1. Install the `.pkg` (Package Installer / GoldHEN / FTP). The package uses
  `CATEGORY=gd` (required so GoldHEN injects plugins).
2. On first launch the app opens the **SETTINGS** menu if no host is set. You can
  also create `/data/moonlight/moonlight.ini` over FTP (see
   `pkg/assets/misc/moonlight.ini`). Optional: `debug_host.txt` with your PC IP.
3. Start Sunshine on the PC. On first pair the app shows a PIN (also written to
  `/data/moonlight/pin.txt`); enter it at `https://<pc>:47990/pin`.
4. Logs on the PC: `nc -ulk 9999` (or enable file log in SETTINGS →
  `/data/moonlight/debug.log`).
5. In the menu: **X/O** start app, **L1/R1** switch APPS/SETTINGS,
  **OPTIONS** quit active Sunshine session, **OPTIONS + Touchpad** (~1 s) exit stream.



### Defaults


| Setting             | Default                                                        |
| ------------------- | -------------------------------------------------------------- |
| Resolution / FPS    | 1920×1080 @ 60                                                 |
| Codec / Bitrate     | **HEVC (H.265)**, 45 Mbps                                      |
| `prefer_hw`         | `true` (Videodec2)                                             |
| `dec_pipeline_depth`| `2` (HEVC path; ONION AUs)                                     |
| `bgra_workers`      | `6`                                                            |
| `slices_per_frame`  | `2`                                                            |
| `prefer_ycbcr`      | `false` (BGRA path; correct colors, SSE2 multi-thread convert) |
| Title ID            | `MLNT00001`                                                    |


Mean decode/convert/present times are printed every second over UDP.
Validation protocol: `[docs/CONSOLE_VALIDATE.md](docs/CONSOLE_VALIDATE.md)`.

Isolated Videodec2 spike: `videodec2_spike = true` in the INI.

## Host CLI (Linux)

```bash
./build-host/moonlight-cli pair 192.168.1.100
./build-host/moonlight-cli list 192.168.1.100
./build-host/moonlight-cli stream 192.168.1.100 --res 1280x720 --time 20
./build-host/moonlight-cli quit 192.168.1.100
```



## Layout

```
cmake/openorbis.cmake           CMake toolchain for PS4
pkg/                            package assets (icon0, sce modules)
scripts/                        env, cross FFmpeg, .pkg packaging
src/
  main.c stream.c config.c      orchestration + INI config
  ui/                           on-console menu (APPS / SETTINGS + OSK)
  gamestream/                   pairing/HTTP/XML over mbedTLS
  audio/audio_orbis.c           Opus + sceAudioOut
  video/renderer_videoout.c     YCbCr/NV12 or BGRA presentation (sceVideoOut)
  video/nv12_blit.c             1088→1080 copy (WB_GARLIC / Onion alias)
  video/decoder_ffmpeg.c        H.264 software (FFmpeg)
  video/decoder_orbis.c         H.264 + HEVC hardware (libSceVideodec2)
  video/hdr10_convert.c         HDR10/AVX1 convert kernels (experimental)
  gamestream/sps.c              SPS fixup (num_ref_frames=1)
  input/input_pad.c             DualShock 4
  orbis/                        VideoOut / Videodec2 / net helpers
  host/main_cli.c               Linux development CLI
plugin/
  ycbcr_unlock/                 GoldHEN PRX (1.63) for YCbCr VideoOut
  ycbcr_kpatch/                 FW 9.00 kernel kpayload (BinLoader)
  kernel_dumper_900/            kernel dump helper for RE
docs/                           console validation + kernel YCbCr RE notes
third_party/
  DEPS                        pinned tags/SHAs (lockfile for setup_deps.sh)
  moonlight-common-c          protocol engine (submodule + Orbis patches)
  mbedtls                     crypto (v3.6.4)
  opus                        audio
  h264bitstream               SPS rewrite
patches/                      Orbis patches applied by scripts/apply_patches.sh
scripts/setup_deps.sh         checkout pins + apply patches
scripts/apply_patches.sh
scripts/refresh_patches.sh    regenerate patches after local dep edits
```
