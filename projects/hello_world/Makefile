# EVO Player - hello_world
#
# Milestone 1: prove Docker -> clang -> SDK -> ELF -> PS5 12.70.
#
#   make            build hello_world.elf
#   make install-elf copy it to output/elf/
#   PS5_HOST=192.168.1.50 make test   build + deploy to the console

include ../common/evo.mk

ELF  := hello_world.elf
SRCS := main.c $(EVO_COMMON_SRCS)

all: $(ELF)

$(ELF): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)
