# ---------------------------------------------------------------------------
# GC-Chiptune: GameCube binary (.dol)
#
#   <devkitPro>/msys2/usr/bin/bash.exe -lc "cd <repo> && make"
#
# Prerequisites: tools/build-libxmp-gc.sh (produces build-gc/libxmp.a)
#                tools/build-libfat-gc.sh (installs libfat into libogc2)
#
# Two possible binaries:
#   make            -> the player (tracks read from the SD card)
#   make APP=bench  -> the CPU measurement bench, tracks embedded with bin2s
#                      (the one that produced the figures of docs/STATUS.md 7;
#                       kept so the measurement can be redone)
#
# Makefile written by hand rather than derived from the devkitPro template: the
# sources come from three separate trees (src/, extern/libxmp, data/) and the
# template's directory magic gets in the way more than it helps here.
# ---------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO not set -- run bash with -l to load devkit-env.sh)
endif

include $(DEVKITPRO)/libogc2/gamecube_rules

APP      ?= player
BUILD    := build-gc

ifeq ($(APP),bench)
TARGET   := gc-chiptune-bench
else ifeq ($(APP),player)
TARGET   := gc-chiptune
else
$(error APP must be either player or bench)
endif

LXDIR    := extern/libxmp

# freetype and its chain (bz2, brotli, png, z) live in the portlibs.
# CAREFUL: gamecube_rules' $(PORTLIBS) is a LIST of directories, not a path --
# and not all of them exist. Hence the $(wildcard): without it, ld asks for a
# missing portlibs/gamecube and gives up.
PORTDIRS := $(wildcard $(PORTLIBS_PATH)/ppc $(PORTLIBS_PATH)/gamecube)

INCLUDE  := -I$(LIBOGC_INC) $(foreach d,$(PORTDIRS),-I$(d)/include) \
            -I$(PORTLIBS_PATH)/ppc/include/freetype2 \
            -Iextern/tsf -I$(LXDIR)/include -I$(BUILD)

COMMON   := -O2 $(MACHDEP) $(INCLUDE)

CFLAGS   := $(COMMON) -Wall
CXXFLAGS := $(COMMON) -std=c++11 -Wall
LDFLAGS  := $(MACHDEP) -Wl,-Map,$(BUILD)/$(TARGET).map
LIBPATHS := -L$(LIBOGC_LIB) $(foreach d,$(PORTDIRS),-L$(d)/lib)
# -lfat BEFORE -logc: libfat calls __io_gcsda and friends, which are defined in
# libogc. The reverse order would leave unresolved symbols.
# -lansnd: the audio output goes through libansnd, not the raw AUDIO_ API (which
# crackled on real hardware) nor libasnd (which copied every block by hand). See
# the header of src/gc/audio_gc.c. Same ordering rule, ansnd builds on logc.
# Produced by tools/build-libansnd-gc.sh.
#
# -lgrrlib and its chain: the interface is in GX (see src/gc/ui.h). GRRLIB is
# packaged for libogc2 and for the GameCube (pacman -S libogc2-grrlib), so there
# is nothing to port. Its link line is less obvious than it looks: the freetype
# here is built with brotli and bzip2, and it pulls in libpng by itself for
# colour glyphs. Omitting any of the three makes the link fail INSIDE freetype,
# not in GRRLIB.
LIBS     := $(BUILD)/libxmp.a -lfat -lansnd \
            -lgrrlib -lfreetype -lbz2 -lbrotlidec -lbrotlicommon -lpng -lz \
            -logc -lm

COBJS    := $(BUILD)/audio_gc.o $(BUILD)/sf2_endian.o
CXXOBJS  := $(BUILD)/backend.o

# The bench embeds its tracks (bin2s); the player reads them from the card.
# Distinct main objects, otherwise switching APP without a clean would link the
# old one.
ifeq ($(APP),bench)
# The soundfont is embedded here and NOWHERE else: since Dolphin does not
# emulate the SD card, this is the only way to measure MIDI on target. The
# player reads it from the card.
# One track per backend: the WORST CASE. Light and "typical" tracks settle
# nothing and cost measurement time each on a console driven by hand. Twelve
# seconds are not enough to describe a track -- that is what led to misjudging
# the V2M (docs/STATUS.md 13).
BINDATA  := worst_xm worst_mid soundfont_sf2
# storage.o: the bench copies its results onto the SD card at the end, for lack
# of a debug console on real hardware.
APPOBJS  := $(BUILD)/main_bench.o $(BUILD)/storage.o \
            $(patsubst %,$(BUILD)/%.o,$(BINDATA))
BINHDRS  := $(patsubst %,$(BUILD)/%.h,$(BINDATA))
else
# THE SOUNDFONT IS NOT EMBEDDED, and that is a measured decision.
#
# It was for a while: nothing left to put on the card, which was tempting. But
# TinySoundFont converts EVERY sample to float at load time (11.0 MB for
# TimGM6mb) and keeps NO pointer to the source block. Read from the card, that
# block is released immediately; in .rodata, never. Embedding therefore cost
# 5.7 MB of permanent RAM out of the console's 24 -- and imposed GPL-2 on the
# binary along the way. See docs/STATUS.md 12.20.
APPOBJS  := $(BUILD)/main_player.o $(BUILD)/storage.o $(BUILD)/playlist.o \
            $(BUILD)/library.o $(BUILD)/index.o $(BUILD)/state.o \
            $(BUILD)/ui.o $(BUILD)/font_gx.o $(BUILD)/font_data.o
BINHDRS  :=
endif

OBJS     := $(APPOBJS) $(COBJS) $(CXXOBJS)

.PHONY: all clean run

all: $(BUILD)/$(TARGET).dol

$(BUILD):
	@mkdir -p $(BUILD)

# --- embedded data -----------------------------------------------------------
# bin2s derives the symbols from the file name: data/worst.xm gives worst_xm,
# worst_xm_end and worst_xm_size. -a 32 aligns on a cache line.
$(BUILD)/worst_xm.o $(BUILD)/worst_xm.h: data/worst.xm | $(BUILD)
	@echo "  bin2s $<"
	@bin2s -a 32 -H $(BUILD)/worst_xm.h $< | $(AS) -o $(BUILD)/worst_xm.o

$(BUILD)/worst_mid.o $(BUILD)/worst_mid.h: data/worst.mid | $(BUILD)
	@echo "  bin2s $<"
	@bin2s -a 32 -H $(BUILD)/worst_mid.h $< | $(AS) -o $(BUILD)/worst_mid.o

# 5.7 MB to assemble: allow a good minute.
$(BUILD)/soundfont_sf2.o $(BUILD)/soundfont_sf2.h: data/soundfont.sf2 | $(BUILD)
	@echo "  bin2s $< (5.7 MB, be patient)"
	@bin2s -a 32 -H $(BUILD)/soundfont_sf2.h $< | $(AS) -o $(BUILD)/soundfont_sf2.o

# --- sources -----------------------------------------------------------------
$(BUILD)/main_player.o: src/gc/main.c | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/main_bench.o: src/gc/bench_main.c $(BINHDRS) | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/storage.o: src/gc/storage.c | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/playlist.o: src/gc/playlist.c | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

# library.o depends on neither the screen nor the audio: that is what lets
# tools/lib_test prove it on a PC before it ever touches the console.
$(BUILD)/library.o: src/gc/library.c | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/index.o: src/gc/index.c | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/state.o: src/gc/state.c | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/ui.o: src/gc/ui.c | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/font_data.o: src/gc/font_data.c | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/font_gx.o: src/gc/font_gx.c | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/audio_gc.o: src/gc/audio_gc.c | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/sf2_endian.o: src/player/sf2_endian.c | $(BUILD)
	@echo "  cc  $<"
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/backend.o: src/player/backend.cpp | $(BUILD)
	@echo "  c++ $<"
	@$(CXX) -c $(CXXFLAGS) -o $@ $<

# --- link --------------------------------------------------------------------
# g++ and not gcc: backend.cpp is C++.
$(BUILD)/$(TARGET).elf: $(OBJS) $(BUILD)/libxmp.a
	@echo "  link $(notdir $@)"
	@$(CXX) $(OBJS) $(LDFLAGS) $(LIBPATHS) $(LIBS) -o $@
	@powerpc-eabi-size $@

$(BUILD)/libxmp.a:
	@echo "FAILED: build-gc/libxmp.a missing. Run tools/build-libxmp-gc.sh first" >&2
	@false

clean:
	rm -rf $(BUILD)/*.o $(BUILD)/*.h $(BUILD)/*.elf $(BUILD)/*.dol $(BUILD)/*.map
