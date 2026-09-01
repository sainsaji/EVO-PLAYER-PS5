# EVO Player - sony_player_test
# Clean implementation targeting libSceAvPlayer playback on PS5

include ../common/evo.mk

ELF  := sony_player_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

LIBS := -lSceSysmodule -lSceUserService -lpthread

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
