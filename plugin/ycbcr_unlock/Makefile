# GoldHEN plugin: ycbcr_unlock.prx
# Requires: OO_PS4_TOOLCHAIN, GOLDHEN_SDK (path to built GoldHEN_Plugins_SDK)

DEBUG_FLAGS := -D__FINAL__=1
ifeq ($(DEBUG),1)
    DEBUG_FLAGS := -D__FINAL__=0
endif

TOOLCHAIN := $(OO_PS4_TOOLCHAIN)
GH_SDK    := $(GOLDHEN_SDK)
OUTDIR    := $(CURDIR)/../bin
SRCDIR    := $(CURDIR)/source

# BISECT: patch bisect level (1..5). Empty = normal build (level 5).
# make BISECT=2  →  ../bin/ycbcr_unlock_m2.prx (intermediates in build/m2)
# FIX: fix strategy (0=passthrough natural tag — DEFAULT 1.49).
# make FIX=2     →  ../bin/ycbcr_unlock_fix2.prx (legacy golden BGRA tag)
# Default (make all): FIX_MODE=0 (do not force +0x08=2; kpatch + BIT_YCC_EXT).
ifneq ($(BISECT),)
    TARGET_PRX := $(OUTDIR)/ycbcr_unlock_m$(BISECT).prx
    INTDIR     := $(CURDIR)/build/m$(BISECT)
    VARIANT_DEF := -DBISECT_LEVEL=$(BISECT) -DFIX_MODE=0
else ifneq ($(FIX),)
    TARGET_PRX := $(OUTDIR)/ycbcr_unlock_fix$(FIX).prx
    INTDIR     := $(CURDIR)/build/fix$(FIX)
    VARIANT_DEF := -DFIX_MODE=$(FIX)
else
    TARGET_PRX := $(OUTDIR)/ycbcr_unlock.prx
    INTDIR     := $(CURDIR)/build
    VARIANT_DEF := -DFIX_MODE=0
endif

TARGET_ELF := $(INTDIR)/ycbcr_unlock.elf

LIBS := -lSceLibcInternal -lSceVideoOut -lGoldHEN_Hook -lkernel -lSceSysmodule

CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c \
          $(DEBUG_FLAGS) $(VARIANT_DEF) -fcolor-diagnostics -Wall \
          -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include \
          -I$(GH_SDK)/include

LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x -e _init --eh-frame-hdr \
           -L$(TOOLCHAIN)/lib -L$(GH_SDK) $(LIBS)

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CC   := clang
    LD   := ld.lld
    CDIR := linux
endif
ifeq ($(UNAME_S),Darwin)
    CC   := /usr/local/opt/llvm/bin/clang
    LD   := /usr/local/opt/llvm/bin/ld.lld
    CDIR := macos
endif

.PHONY: all clean bisect

all: $(TARGET_PRX)

# Build all 5 bisect variants: ../bin/ycbcr_unlock_m1.prx .. _m5.prx
bisect:
	@for lvl in 1 2 3 4 5; do \
	    echo "=== BISECT level $$lvl ==="; \
	    $(MAKE) --no-print-directory BISECT=$$lvl $(OUTDIR)/ycbcr_unlock_m$$lvl.prx || exit 1; \
	done
	@echo "OK: bisect m1..m5 in $(OUTDIR)"
$(INTDIR):
	mkdir -p $(INTDIR) $(OUTDIR)

$(INTDIR)/main.o: $(SRCDIR)/main.c | $(INTDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(TARGET_ELF): $(INTDIR)/main.o
	$(LD) $(GH_SDK)/build/crtprx.o $(INTDIR)/main.o -o $@ $(LDFLAGS)

$(TARGET_PRX): $(TARGET_ELF)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself \
	    -in=$< -out=$(INTDIR)/ycbcr_unlock.oelf \
	    --lib=$@ --paid 0x3800000000000011
	@echo "OK: $@"

clean:
	rm -rf $(CURDIR)/build $(OUTDIR)/ycbcr_unlock.prx \
	    $(OUTDIR)/ycbcr_unlock_m*.prx $(OUTDIR)/ycbcr_unlock_fix*.prx
