# EVO Player Media home launcher
# core/ = websrv BigApp path (John Törnblom, GPL-3.0+)
# orchestration = ProsperoPlayer original, rebranded for EVO Player
#
# Do not call this directly - use ./scripts/build-media-tile.sh, which stages
# the player ELF into assets/ first. This Makefile embeds assets/EVOPlayer.elf
# whole, and cannot build without it.

SDK ?= /opt/ps5-payload-sdk
CC := $(SDK)/bin/prospero-clang
STRIP := $(SDK)/bin/prospero-strip

ELF := EVOPlayer_MediaLauncher.elf
UNINSTALL_ELF := EVOPlayer_UninstallTile.elf

PLAYER := assets/EVOPlayer.elf

SRCS := \
	prospero_media_launcher.c \
	core/hbldr.c \
	core/elfldr.c \
	core/pt.c

CFLAGS := -Os -Wall -Wno-unused-parameter \
	-I. -Icore \
	-I$(SDK)/target/include

LIBS := \
	-lSceIpmi \
	-lSceUserService \
	-lSceAppInstUtil \
	-lSceSystemService \
	-lSceNet \
	-lpthread

all: $(ELF) $(UNINSTALL_ELF)

# The player is embedded with .incbin, which the compiler resolves relative to
# this directory - so a missing asset surfaces as an assembler error about a
# file it cannot open. Say what is actually wrong instead.
$(PLAYER):
	@echo "$(PLAYER) is missing - the launcher embeds the player whole."; \
	 echo "Stage it first:"; \
	 echo "    ./scripts/build-media-tile.sh"; \
	 echo "or by hand:"; \
	 echo "    cp output/elf/EVOPlayer.elf $(PLAYER)"; \
	 exit 1

$(ELF): $(SRCS) $(PLAYER) assets/param.json assets/icon0.png
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)
	$(STRIP) $@
	@sha256sum $@
	@ls -lh $@

$(UNINSTALL_ELF): prospero_uninstall_tile.c
	$(CC) $(CFLAGS) -o $@ prospero_uninstall_tile.c $(LIBS)
	$(STRIP) $@
	@sha256sum $@
	@ls -lh $@

clean:
	rm -f $(ELF) $(UNINSTALL_ELF)

.PHONY: all clean
