# sandbox_unjail - elfldr payload that lifts the app sandbox on the running
# EVO Player process. Phase 1b milestone-1 task 7.
#
#   PS5_HOST=192.168.0.10 make test      build + push to elfldr (port 9021)
#
# See docs/evo-pro/phase-1b-app-module.md section 5, Step B.2.
include ../common/evo.mk

ELF  := sandbox_unjail.elf
SRCS := main.c $(EVO_COMMON_SRCS)

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)
