# EVO Player - computequeue_test
#
# Phase 4 of the hardware-decode work: the compute memory requirement, the
# direct-memory ceiling a payload actually has, and sceVideodec2AllocateCompute-
# Queue - the first allocating call in the whole effort.
#
# Kept separate from videodec2_test so that Phase 3's evidence stays
# reproducible on its own, and a dead end here costs nothing there.
#
# No -lSceVideodec2: the SDK ships no stub for it. The module is loaded with
# sceKernelLoadStartModule and every entry point resolved by NID at run time.
# The direct-memory calls come from libkernel, which the CRT links by default.

include ../common/evo.mk

ELF  := computequeue_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

LIBS := -lSceSysmodule

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
