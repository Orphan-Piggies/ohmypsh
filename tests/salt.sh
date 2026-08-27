#!/bin/sh
# Smoke tests for salt, the armory's cat.
# The founding rule gets the founding test: piped output is the
# input, byte for byte. Everything else is decoration.
# Usage: sh tests/salt.sh   (or: make test)

cd "$(dirname "$0")/.." || exit 1
SALT=${SALT:-./salt}
fails=0

check() {
    desc=$1 expected=$2 actual=$3
    if [ "$actual" = "$expected" ]; then
        printf 'ok   %s\n' "$desc"
    else
        printf 'FAIL %s\n     expected: %s\n     got:      %s\n' \
            "$desc" "$expected" "$actual"
        fails=$((fails + 1))
    fi
}

tmp=$(mktemp -d)
E=$(printf '\033')  # a literal escape byte, for grepping

# ---- the founding rule: byte identity when piped ----

for f in src/psh.h README.md omp/plugins/redis.psh Makefile docs/psh.1; do
    if $SALT "$f" | cmp -s - "$f"; then
        printf 'ok   byte-identical when piped: %s\n' "$f"
    else
        printf 'FAIL byte-identity broken: %s\n' "$f"
        fails=$((fails + 1))
    fi
done

printf 'no trailing newline' > "$tmp/nonl"
if $SALT "$tmp/nonl" | cmp -s - "$tmp/nonl"; then
    printf 'ok   missing final newline preserved\n'
else
    printf 'FAIL missing final newline mangled\n'
    fails=$((fails + 1))
fi

# even with color forced, stripping the ANSI gives back the source
$SALT -c src/exec.c | sed "s/$E\[[0-9;]*m//g" | cmp -s - src/exec.c \
    && printf 'ok   colors are the ONLY difference (-c, stripped)\n' \
    || { printf 'FAIL -c output is more than colors\n'; fails=$((fails + 1)); }

# ---- color plumbing ----

n=$($SALT src/exec.c | grep -c "$E" || true)
check "no escapes when stdout is a pipe" "0" "$n"

$SALT -c src/exec.c | grep -q "$E" \
    && printf 'ok   -c forces color into a pipe\n' \
    || { printf 'FAIL -c produced no color\n'; fails=$((fails + 1)); }

n=$($SALT -c -p src/exec.c | grep -c "$E" || true)
check "-p wins: no color ever" "0" "$n"

# ---- language selection ----

out=$(printf 'return 1\n' | $SALT -c -l py | grep -c "$E\[1m")
check "-l forces a language on stdin (py keyword bold)" "1" "$out"

out=$(printf 'x\n' | $SALT -c -l nope 2>/dev/null; echo $?)
check "unknown -l is an error (status 2)" "2" "$out"

$SALT -L | grep -q elixir \
    && printf 'ok   -L lists the arsenal\n' \
    || { printf 'FAIL -L is missing languages\n'; fails=$((fails + 1)); }

printf '#!/usr/bin/env python3\nimport os\n' > "$tmp/noext"
out=$($SALT -c "$tmp/noext" | grep -c "$E\[1mimport")
check "shebang detection without an extension" "1" "$out"

# ---- line numbers (opt-in) ----

out=$(printf 'a\nb\n' | $SALT -n)
check "-n numbers lines" "    1  a
    2  b" "$out"

# ---- guard rails ----

printf 'text\0binary' > "$tmp/bin"
$SALT "$tmp/bin" >/dev/null 2>&1
check "binary input is refused (status 1)" "1" "$?"

$SALT "$tmp/definitely-missing" 2>/dev/null
check "missing file is status 1" "1" "$?"

# ---- markdown fences highlight their language ----

printf '# T\n```c\nreturn 0;\n```\n' > "$tmp/f.md"
out=$($SALT -c "$tmp/f.md" | grep -c "$E\[1mreturn")
check "md fence gets C keywords" "1" "$out"

rm -rf "$tmp"

echo
if [ "$fails" -eq 0 ]; then
    echo "salt passes — the bytes were never harmed 🧂"
else
    echo "$fails salt test(s) failed"
    exit 1
fi
