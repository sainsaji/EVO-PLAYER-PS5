# EVO Player - gpu_test
#
# Milestone 5: establish what GPU access actually exists on this SDK.
# This is a capability probe, not a renderer - see main.c for why raw GNM is
# not the productive first route, and docs/gpu-notes.md for the full audit.

include ../common/evo.mk

ELF  := gpu_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

# -lSceGnmDriver  sceGnm* (stub: sce_stubs/libSceGnmDriver.c)
#                 A libSceGnmDriverForNeoMode stub also exists; it is the
#                 PS4-Pro-mode variant and is not what a PS5 payload wants.
LIBS := -lSceGnmDriver

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
