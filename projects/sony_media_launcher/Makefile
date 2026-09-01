# EVO Player - sony_media_launcher
# Launches the official Sony Media Gallery / Player with target video

include ../common/evo.mk

ELF  := sony_media_launcher.elf
SRCS := main.c $(EVO_COMMON_SRCS)

LIBS := -lSceSystemService -lSceUserService -lSceSysmodule -lpthread

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
