# psh — the pistachio shell 🫛
#
# Targets:
#   make        build ./psh (debug build with all warnings)
#   make test   build and run the smoke tests
#   make clean  remove build artifacts

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -g -D_POSIX_C_SOURCE=200809L
LDLIBS  :=  # zero dependencies since H4.4 — libc is the whole ship

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

all: psh salt

psh: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/psh.h
	$(CC) $(CFLAGS) -c -o $@ $<

# salt — the armory's cat: colors, never changed bytes (tools/salt.c)
salt: tools/salt.c
	$(CC) $(CFLAGS) -o $@ tools/salt.c $(LDLIBS)

test: psh salt
	sh tests/smoke.sh
	sh tests/salt.sh

# Cockpit keystroke tests: need a pty (script(1)); run locally.
test-editor: psh
	sh tests/editor.sh

# AddressSanitizer + LeakSanitizer build: memory errors and leaks
# surface as loud reports. `make test-asan` runs the whole suite on it.
psh-asan: $(SRC) src/psh.h
	$(CC) $(CFLAGS) -fsanitize=address,leak -o $@ $(SRC) $(LDLIBS)

salt-asan: tools/salt.c
	$(CC) $(CFLAGS) -fsanitize=address,leak -o $@ tools/salt.c $(LDLIBS)

test-asan: psh-asan salt-asan
	PSH=./psh-asan sh tests/smoke.sh
	SALT=./salt-asan sh tests/salt.sh

PREFIX ?= /usr/local

install: psh salt
	install -m 755 psh $(PREFIX)/bin/psh
	install -m 755 salt $(PREFIX)/bin/salt
	install -d $(PREFIX)/share/man/man1
	install -m 644 docs/psh.1 $(PREFIX)/share/man/man1/psh.1
	install -m 644 docs/salt.1 $(PREFIX)/share/man/man1/salt.1

clean:
	rm -f psh psh-asan salt salt-asan $(OBJ)

.PHONY: all test test-editor test-asan install clean
