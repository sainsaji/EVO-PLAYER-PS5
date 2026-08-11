# EVO Player - slotcheck
#
# Answers "is the app slot free?" without launching into it.
#
# DEPLOY WITH ./scripts/deploy.sh, NOT install-homebrew.sh --run. deploy.sh
# lands in SceSpZeroConf, which is not the app slot, so this cannot stack on
# whatever is already there. That inversion of the usual rule is the point.

include ../common/evo.mk

ELF  := slotcheck.elf
SRCS := main.c $(EVO_COMMON_SRCS)

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)
