# Kernel dumper FW 9.00 (port of [VV1LD/PS4-KernelDumper](https://github.com/VV1LD/PS4-KernelDumper))

GoldHEN BinLoader `:9090`. **Trap 12 risk** same as our kdump; not magic.

## Build

```bash
export PS4SDK=…/ps4-payload-sdk
make -C plugin/kernel_dumper_900          # → ../bin/kernel_dumper_900.bin (file)
make -C plugin/kernel_dumper_900 socket   # → …_sock.bin (TCP to PC)
```

## File usage (default)

Output: `/data/moonlight/kernel_900.bin` (13 MiB text). Orbis page **16 KiB**.

```bash
# Only when the user explicitly asks:
nc -w 3 <PS4_IP> 9090 < plugin/bin/kernel_dumper_900.bin
# Notifs: patched → dumping… → N MiB → file OK
# FTP :2121 → PC
```

## Socket usage

1. In `include/defines.h` or `socket` build: PC IP (`DUMP_IP`, default `192.168.0.91`) port `9023`
2. PC: `socat -u TCP-LISTEN:9023,reuseaddr,fork - > kernel_900.bin`
3. Send the `_sock.bin` to `:9090`

## Offsets 9.00

| Define | Value |
|--------|-------|
| `KERN_BASE_PTR` | `0x1C0` |
| `KERN_COPYOUT` | `0x2715B0` |
| `KERN_PRISON0` | `0x111F870` |
| `KERN_ROOTVNODE` | `0x21EFF20` |

`KERN_DUMPSIZE` default `0xD00000` (override `-DKERN_DUMPSIZE=…`).
