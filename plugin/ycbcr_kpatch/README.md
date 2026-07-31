# YCbCr VideoOut kpayload (FW 9.00)

| Bin | Build | Use |
|-----|-------|-----|
| `ycbcr_kpatch_900.bin` | `make` (MODE=0) | safe ping |
| `ycbcr_kdump_900.bin` | `make dump DUMP_OFF=… DUMP_LEN=…` | island (legacy; full dump already OK) |
| apply | `EXTRA_CFLAGS='-DMODE=1'` | NOP @ `0x50ea26` |

## Fixed offset (full VV1LD dump)

`YCBCR_KPATCH_FIXED_OFF = 0x50ea26` — `jne` to EINVAL after the pixel-format
whitelist (YCbCr `0x08322200` is not in the list; BGRA is). Details:
`docs/KERNEL_YCBCR_RE.md`.

```bash
export PS4SDK=…/ps4-payload-sdk
make -C plugin/ycbcr_kpatch EXTRA_CFLAGS='-DMODE=1'
# Only when the user asks:
nc -w 3 <PS4_IP> 9090 < plugin/bin/ycbcr_kpatch_900.bin
```

Full dump: `pkg/assets/misc/kernel_900.bin`. Do not resume MODE=3 / island
sweeps unless needed.

## Ping

```bash
nc -w 3 <PS4_IP> 9090 < plugin/bin/ycbcr_kpatch_900.bin
```
