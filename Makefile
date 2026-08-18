# psh — the pistachio shell 🫛
#
# Targets:
#   make        build ./psh (debug build with all warnings)
#   make test   build and run the smoke tests
#   make clean  remove build artifacts

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -g -D_POSIX_C_SOURCE=200809L
LDLIBS  := -lreadline

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

psh: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/psh.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: psh
	sh tests/smoke.sh

clean:
	rm -f psh $(OBJ)

.PHONY: test clean
