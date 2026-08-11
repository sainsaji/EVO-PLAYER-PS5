# EVO Player - codecdump_test
#
# Phase 6's impasse-breaker: create an H.264 decoder, diff the module table,
# and dump whatever CreateDecoder loaded on demand.
#
# The submit that refuses the decode lives in a module that is not in
# proprietary/dump/ - sceVdecCoreCreateDecoder pulls it in through
# sceSysmoduleLoadModuleInternal (internal id 0x80000036 for H.264). This
# probe is how it gets dumped, so the submit path becomes an offline objdump
# question like every gate before it.
#
# It never calls Decode, so it carries Phase 5's risk profile, not Phase 6's.
#
# LAUNCH IT WITH ./scripts/install-homebrew.sh --run, NOT ./scripts/deploy.sh.
# The 1080p working set is 90 MiB; the elfldr slot caps out below 64.
#
# No -lSceVideodec2: the SDK ships no stub for it. The module is loaded with
# sceKernelLoadStartModule and every entry point resolved by NID at run time.

include ../common/evo.mk

ELF  := codecdump_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

LIBS := -lSceSysmodule

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
