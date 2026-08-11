# EVO Player - createdecoder_test
#
# Phase 5 of the hardware-decode work: hand the module the buffers its own
# query asked for and call sceVideodec2CreateDecoder.
#
# Kept separate from computequeue_test so that Phase 4's evidence stays
# reproducible on its own, and a dead end here costs nothing there.
#
# LAUNCH IT WITH ./scripts/install-homebrew.sh --run, NOT ./scripts/deploy.sh.
# The 1080p working set is 90 MiB; the elfldr slot caps out below 64.
#
# No -lSceVideodec2: the SDK ships no stub for it. The module is loaded with
# sceKernelLoadStartModule and every entry point resolved by NID at run time.
# The direct-memory calls come from libkernel, which the CRT links by default.

include ../common/evo.mk

ELF  := createdecoder_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

LIBS := -lSceSysmodule

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
