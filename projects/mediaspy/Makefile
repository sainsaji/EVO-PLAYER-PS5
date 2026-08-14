# EVO Player - mediaspy
#
# Reads the credentials of a process that is actually decoding video, so the
# errno 5200 identity question can be settled by comparison instead of guesswork.
#
# DEPLOY WITH ./scripts/deploy.sh, NOT install-homebrew.sh --run. --run borrows
# the PS Now application slot, and the Media Gallery being observed IS an
# application - launching into that slot would evict the process this is meant
# to measure. deploy.sh lands in SceSpZeroConf and only watches.

include ../common/evo.mk

ELF  := mediaspy.elf
SRCS := main.c $(EVO_COMMON_SRCS)

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)
