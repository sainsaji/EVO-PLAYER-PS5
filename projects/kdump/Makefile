# EVO Player - kdump
#
# Dump the running PS5 kernel to the USB stick, once, so every later question
# about it becomes an offline objdump question.
#
# DEPLOY WITH ./scripts/deploy.sh, NOT install-homebrew.sh --run.
# It lands in SceSpZeroConf, never touches the PS Now app slot, and so cannot
# stack on anything. It needs no app-slot memory budget - the dump is written
# straight to /mnt/usb0 in chunks.
#
# READ-ONLY. kernel_copyout and the ucred getters only. No ioctls, no driver
# calls, no video out, no kernel writes. Standing rule 18 exists because the
# last payload that reached a kernel driver panicked the console; nothing here
# goes near one.

include ../common/evo.mk

ELF  := kdump.elf
SRCS := main.c $(EVO_COMMON_SRCS)

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)
