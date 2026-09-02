# avplayer_test - libSceAvPlayer native decode spike (Phase 2, Route A).
#
# See main.c and docs/evo-pro/native-decode-plan.md (Phase 2).
#
#   make test PS5_HOST=<ip>                 decode /data/bunny.mp4
#   make test PS5_HOST=<ip> ARGS=/data/x.mp4    (argv[1] overrides the file)
#
# No -lSceAvPlayer: the SDK ships no such stub. Every entry point is resolved
# at run time by NID (nid_encode + kernel_dynlib_resolve). Nothing here opens
# VideoOut, so the sceVideoOutOpen panic vector is not in play.
#   -lkernel   : direct-memory + module-load calls
#   -lpthread  : watchdog thread that force-exits on a hang
include ../common/evo.mk

ELF  := avplayer_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

# The Phase 0 ABI header lives with the player's media module.
CFLAGS += -I$(EVO_ROOT)/projects/evoplayer/media/include

LIBS := -lSceSysmodule -lkernel -lpthread

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
