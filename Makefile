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

all: psh salt roast

psh: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/psh.h
	$(CC) $(CFLAGS) -c -o $@ $<

# the armory: salt colors bytes, roast renders Markdown (tools/)
salt: tools/salt.c tools/hl.h
	$(CC) $(CFLAGS) -o $@ tools/salt.c $(LDLIBS)

roast: tools/roast.c tools/hl.h
	$(CC) $(CFLAGS) -o $@ tools/roast.c $(LDLIBS)

test: psh salt roast
	sh tests/smoke.sh
	sh tests/salt.sh
	sh tests/roast.sh

# Cockpit keystroke tests: need a pty (script(1)); run locally.
test-editor: psh
	sh tests/editor.sh

# AddressSanitizer + LeakSanitizer build: memory errors and leaks
# surface as loud reports. `make test-asan` runs the whole suite on it.
psh-asan: $(SRC) src/psh.h
	$(CC) $(CFLAGS) -fsanitize=address,leak -o $@ $(SRC) $(LDLIBS)

salt-asan: tools/salt.c tools/hl.h
	$(CC) $(CFLAGS) -fsanitize=address,leak -o $@ tools/salt.c $(LDLIBS)

roast-asan: tools/roast.c tools/hl.h
	$(CC) $(CFLAGS) -fsanitize=address,leak -o $@ tools/roast.c $(LDLIBS)

test-asan: psh-asan salt-asan roast-asan
	PSH=./psh-asan sh tests/smoke.sh
	SALT=./salt-asan sh tests/salt.sh
	ROAST=./roast-asan sh tests/roast.sh

PREFIX ?= /usr/local

install: psh salt roast
	install -m 755 psh $(PREFIX)/bin/psh
	install -m 755 salt $(PREFIX)/bin/salt
	install -m 755 roast $(PREFIX)/bin/roast
	install -d $(PREFIX)/share/man/man1
	install -m 644 docs/psh.1 $(PREFIX)/share/man/man1/psh.1
	install -m 644 docs/salt.1 $(PREFIX)/share/man/man1/salt.1
	install -m 644 docs/roast.1 $(PREFIX)/share/man/man1/roast.1

clean:
	rm -f psh psh-asan salt salt-asan roast roast-asan $(OBJ)

.PHONY: all test test-editor test-asan install clean
