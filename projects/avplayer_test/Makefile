# EVO Player - avplayer_test  (RESEARCH SCAFFOLD, not implemented)
#
# Reserved for libSceAvPlayer experiments. Builds a placeholder ELF so the
# project stays wired into the build and CI from day one.
# No -lSceAvPlayer: the SDK ships no such stub (verified in v0.42).

include ../common/evo.mk

ELF  := avplayer_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

LIBS := -lSceSysmodule

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
