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

# AddressSanitizer + LeakSanitizer build: memory errors and leaks
# surface as loud reports. `make test-asan` runs the whole suite on it.
psh-asan: $(SRC) src/psh.h
	$(CC) $(CFLAGS) -fsanitize=address,leak -o $@ $(SRC) $(LDLIBS)

test-asan: psh-asan
	PSH=./psh-asan sh tests/smoke.sh

PREFIX ?= /usr/local

install: psh
	install -m 755 psh $(PREFIX)/bin/psh
	install -d $(PREFIX)/share/man/man1
	install -m 644 docs/psh.1 $(PREFIX)/share/man/man1/psh.1

clean:
	rm -f psh psh-asan $(OBJ)

.PHONY: test test-asan install clean
