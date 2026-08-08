# EVO Player - audioout_test
#
# Milestone 4: AudioOut init, 48 kHz stereo S16, PCM generation, sine playback.

include ../common/evo.mk

ELF  := audioout_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

# -lSceAudioOut     sceAudioOut* (stub: sce_stubs/libSceAudioOut.c)
# -lSceUserService  sceAudioOutOpen takes a user id
# -lm               sin() for the tone generator
LIBS := -lSceAudioOut -lSceUserService -lm

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
