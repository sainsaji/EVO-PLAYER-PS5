# EVO Player - media_watcher
# Background watcher that monitors official Sony Media Player and pops notifications

include ../common/evo.mk

ELF  := media_watcher.elf
SRCS := main.c $(EVO_COMMON_SRCS)

LIBS := -lSceSysmodule -lSceUserService -lpthread

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(ELF)
