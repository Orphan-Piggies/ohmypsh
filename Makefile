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

all: psh salt roast cage

psh: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/psh.h
	$(CC) $(CFLAGS) -c -o $@ $<

# the armory: salt colors bytes, roast renders Markdown (tools/)
salt: tools/salt.c tools/hl.h
	$(CC) $(CFLAGS) -o $@ tools/salt.c $(LDLIBS)

roast: tools/roast.c tools/hl.h
	$(CC) $(CFLAGS) -o $@ tools/roast.c $(LDLIBS)

# cage — the sandbox: Landlock, pure syscalls (tools/cage.c)
cage: tools/cage.c
	$(CC) $(CFLAGS) -o $@ tools/cage.c $(LDLIBS)

test: psh salt roast cage
	sh tests/smoke.sh
	sh tests/salt.sh
	sh tests/roast.sh
	sh tests/cage.sh

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

cage-asan: tools/cage.c
	$(CC) $(CFLAGS) -fsanitize=address,leak -o $@ tools/cage.c $(LDLIBS)

test-asan: psh-asan salt-asan roast-asan cage-asan
	PSH=./psh-asan sh tests/smoke.sh
	SALT=./salt-asan sh tests/salt.sh
	ROAST=./roast-asan sh tests/roast.sh
	CAGE=./cage-asan sh tests/cage.sh

PREFIX ?= /usr/local
DESTDIR ?=

# DESTDIR supports staged installs — packaging (deb, PKGBUILD) uses
# it; a plain `make install` still lands in /usr/local. The omp
# framework ships too: point OMP_DIR at share/psh/omp in ~/.pshrc.
install: psh salt roast cage
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 psh salt roast cage $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 644 docs/psh.1 docs/salt.1 docs/roast.1 docs/cage.1 \
	    $(DESTDIR)$(PREFIX)/share/man/man1
	install -d $(DESTDIR)$(PREFIX)/share/psh
	cp -r omp $(DESTDIR)$(PREFIX)/share/psh/

# A binary .deb, no debhelper ceremony: stage via install, add the
# DEBIAN control files, dpkg-deb. `make deb` → ohmypsh_V_ARCH.deb
VERSION := $(shell sed -n 's/\#define PSH_VERSION "\(.*\)"/\1/p' src/psh.h)
DEBARCH := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)

deb: all
	rm -rf debstage
	$(MAKE) install DESTDIR=debstage PREFIX=/usr
	strip debstage/usr/bin/psh debstage/usr/bin/salt \
	    debstage/usr/bin/roast debstage/usr/bin/cage
	gzip -9n debstage/usr/share/man/man1/*.1
	install -d debstage/usr/share/doc/ohmypsh
	install -m 644 LICENSE debstage/usr/share/doc/ohmypsh/copyright
	install -d debstage/DEBIAN
	sed -e 's/@VERSION@/$(VERSION)/' -e 's/@ARCH@/$(DEBARCH)/' \
	    packaging/deb/control.in > debstage/DEBIAN/control
	install -m 755 packaging/deb/postinst packaging/deb/postrm \
	    debstage/DEBIAN
	dpkg-deb --build --root-owner-group debstage \
	    ohmypsh_$(VERSION)_$(DEBARCH).deb
	rm -rf debstage

clean:
	rm -f psh psh-asan salt salt-asan roast roast-asan cage cage-asan \
	    $(OBJ) ohmypsh_*.deb
	rm -rf debstage

.PHONY: all test test-editor test-asan install deb clean
