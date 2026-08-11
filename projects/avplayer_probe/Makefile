# EVO Player - avplayer_probe
#
# Route A, as an instrument: every AvPlayer callback logs its arguments, and
# the run settles whether sceVideoDecoderArbitrationInitialize - which blocks
# for Route B - answers once AvPlayer has been initialised.
#
# RUN slotcheck (via deploy.sh) FIRST to confirm the app slot is clear, then
# launch this with ./scripts/install-homebrew.sh --run.
#
# test_clip.S links a 720p MP4 into .rodata; the file callbacks serve it from
# there, so nothing is copied onto the console. -I$(CURDIR) is what lets the
# assembler find the .mp4 next to the .S.

include ../common/evo.mk

ELF  := avplayer_probe.elf
SRCS := main.c test_clip.S $(EVO_COMMON_SRCS)

CFLAGS += -I$(CURDIR)

LIBS := -lSceSysmodule

all: $(ELF)

$(ELF): $(SRCS) test_720p.mp4
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)

clean:
	rm -f $(ELF)
