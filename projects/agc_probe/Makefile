# agc_probe - plain elfldr-payload build.
#
# Gate question for the GPU rendering track Step 2: can a payload reach sceAgc?
# See main.c and docs/evo-pro/gpu-rendering-plan.md.
#
#   make test PS5_HOST=<ip>              Phase A only (recon, zero GPU risk)
#   make test PS5_HOST=<ip> AGC_INIT=1   Phase A + sceAgcInit (one risky call)
include ../common/evo.mk

ELF  := agc_probe.elf
SRCS := main.c $(EVO_COMMON_SRCS)

ifdef AGC_INIT
CFLAGS += -DAGC_PROBE_INIT=1
endif

# No GPU stub linked - touching libSceGnmDriver from an elfldr payload crashes
# the process. sceKernelLoadStartModule + dynlib resolution need nothing here.
# -lpthread : watchdog thread that force-exits on a hang (see main.c).
LIBS := -lSceSysmodule -lkernel -lpthread

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
