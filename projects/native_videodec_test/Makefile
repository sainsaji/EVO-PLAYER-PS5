# PS5 Native Hardware Video Decoder Test Makefile
include ../common/evo.mk

ELF  := native_videodec_test.elf
SRCS := main.c $(EVO_COMMON_SRCS)

LIBS := -lSceVideoOut -lSceGnmDriver -lSceSysmodule -lSceSystemService -lkernel

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
