# app_ctl - hbldr/elfldr payload to launch/kill the PPSA99039 app module.
include ../common/evo.mk

ELF  := app_ctl.elf
SRCS := main.c $(EVO_COMMON_SRCS)

LIBS := -lSceSystemService -lSceUserService -lkernel

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
