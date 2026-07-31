# ycbcr_kpatch — .bin payload for GoldHEN BinLoader (port 9090), FW 9.00
#
# Requires: PS4SDK pointing at Scene-Collective ps4-payload-sdk (built libPS4).
#   export PS4SDK=$HOME/ps4dev/ps4-payload-sdk   # or /tmp/ps4-payload-sdk
#
# Output: ../bin/ycbcr_kpatch_900.bin

LIBPS4  := $(PS4SDK)/libPS4

CC      := gcc
OBJCOPY := objcopy
ODIR    := build
SDIR    := source
IDIRS   := -I$(LIBPS4)/include -Iinclude
LDIRS   := -L$(LIBPS4)
# Optional: EXTRA_CFLAGS='-DAPPLY_SCAN=1' or '-DYCBCR_KPATCH_FIXED_OFF=0x...'
CFLAGS  := $(IDIRS) -Os -std=c11 -ffunction-sections -fdata-sections \
           -fno-builtin -nostartfiles -nostdlib -Wall -Wextra -masm=intel \
           -march=btver2 -mtune=btver2 -m64 -mabi=sysv -mcmodel=small -fpie -fPIC \
           $(EXTRA_CFLAGS)
LFLAGS  := $(LDIRS) -Xlinker -T $(LIBPS4)/linker.x -Wl,--build-id=none -Wl,--gc-sections
CFILES  := $(wildcard $(SDIR)/*.c)
OBJS    := $(patsubst $(SDIR)/%.c, $(ODIR)/%.o, $(CFILES))
LIBS    := -lPS4

OUTDIR  := $(CURDIR)/../bin
TARGET  := $(OUTDIR)/ycbcr_kpatch_900.bin
DUMPBIN := $(OUTDIR)/ycbcr_kdump_900.bin

.PHONY: all clean check-sdk dump

all: check-sdk $(TARGET)

# Kernel dump → /data/moonlight/kernel_900.bin (MODE=3)
# Example: make dump DUMP_OFF=0 DUMP_LEN=0x10000
DUMP_OFF ?= 0
DUMP_LEN ?= 0x10000

dump: check-sdk
	rm -rf build
	$(MAKE) EXTRA_CFLAGS='-DMODE=3 -DDUMP_OFF=$(DUMP_OFF)ull -DDUMP_LEN=$(DUMP_LEN)ull' \
		TARGET='$(DUMPBIN)' $(DUMPBIN)

check-sdk:
	@test -n "$(PS4SDK)" || (echo "Set PS4SDK to ps4-payload-sdk root"; exit 1)
	@test -f "$(LIBPS4)/libPS4.a" || (echo "Missing $(LIBPS4)/libPS4.a — run: make -C $(LIBPS4)"; exit 1)

$(TARGET): $(ODIR) $(OBJS) | $(OUTDIR)
	$(CC) $(LIBPS4)/crt0.s $(ODIR)/*.o -o $(ODIR)/temp.t $(CFLAGS) $(LFLAGS) $(LIBS)
	$(OBJCOPY) -O binary $(ODIR)/temp.t "$@"
	@rm -f $(ODIR)/temp.t
	@ls -la "$@"

$(ODIR)/%.o: $(SDIR)/%.c | $(ODIR)
	$(CC) -c -o $@ $< $(CFLAGS)

$(ODIR) $(OUTDIR):
	mkdir -p $@

clean:
	rm -rf $(ODIR) $(TARGET)
