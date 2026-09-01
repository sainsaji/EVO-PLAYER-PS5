# sandbox_probe - plain elfldr-payload build.
#
# This is the A/B reference: the SAME probe run from the unsandboxed
# hbldr/elfldr context. Compare its mount visibility against the app-module
# build produced by scripts/package-app.sh --probe.
include ../common/evo.mk

ELF  := sandbox_probe.elf
SRCS := main.c $(EVO_COMMON_SRCS)

LIBS := -lSceUserService -lkernel

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
