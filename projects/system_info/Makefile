# EVO Player - system_info
#
# Milestone 2: libkernel, firmware reporting, CPU and memory info.
# Use this to *prove* the console is on 12.70 before trusting anything else.

include ../common/evo.mk

ELF  := system_info.elf
SRCS := main.c $(EVO_COMMON_SRCS)

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)
