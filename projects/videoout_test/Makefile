# EVO Player - videoout_test
#
# Milestone 3: VideoOut init, framebuffer allocation, buffer registration,
# flip and presentation.
#
#   make
#   PS5_HOST=192.168.1.50 make test
#
# The payload takes an optional argument:
#   (none)    cycle solid colours
#   pattern   draw the RGB test pattern
# Pass it through the loader that supports argv (websrv), or edit main().

include ../common/evo.mk

ELF  := videoout_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

# -lSceVideoOut       sceVideoOut* (stub: sce_stubs/libSceVideoOut.c)
# -lSceSystemService  sceSystemServiceHideSplashScreen
# libkernel's direct-memory calls are linked by default via the CRT.
#
# Note: no -lSceUserService. A payload has no user session, so we pass user id
# 0xff (system) to sceVideoOutOpen rather than asking UserService - see the
# comment block at the top of main.c.
LIBS := -lSceVideoOut -lSceSystemService

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
