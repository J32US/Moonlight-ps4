# VV1LD KernelDumper port → FW 9.00, BinLoader output (.bin)
# Requires: export PS4SDK=…/ps4-payload-sdk

LIBPS4  := $(PS4SDK)/libPS4
CC      := gcc
OBJCOPY := objcopy
ODIR    := build
SDIR    := source
IDIRS   := -I$(LIBPS4)/include -Iinclude
LDIRS   := -L$(LIBPS4)
CFLAGS  := $(IDIRS) -Os -std=c11 -ffunction-sections -fdata-sections \
           -fno-builtin -nostartfiles -nostdlib -Wall -Wextra -masm=intel \
           -march=btver2 -mtune=btver2 -m64 -mabi=sysv -mcmodel=small -fpie -fPIC \
           $(EXTRA_CFLAGS)
LFLAGS  := $(LDIRS) -Xlinker -T $(LIBPS4)/linker.x -Wl,--build-id=none -Wl,--gc-sections
CFILES  := $(wildcard $(SDIR)/*.c)
OBJS    := $(patsubst $(SDIR)/%.c, $(ODIR)/%.o, $(CFILES))
LIBS    := -lPS4

OUTDIR  := $(CURDIR)/../bin
TARGET  := $(OUTDIR)/kernel_dumper_900.bin

.PHONY: all clean check-sdk socket

all: check-sdk $(TARGET)

SOCKBIN := $(OUTDIR)/kernel_dumper_900_sock.bin

socket: check-sdk
	rm -rf build
	$(MAKE) EXTRA_CFLAGS='-DDEBUG_SOCKET' TARGET='$(SOCKBIN)' $(SOCKBIN)

check-sdk:
	@test -n "$(PS4SDK)" || (echo "Set PS4SDK"; exit 1)
	@test -f "$(LIBPS4)/libPS4.a" || (echo "Missing libPS4.a"; exit 1)

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
	rm -rf $(ODIR) $(TARGET) $(SOCKBIN)
