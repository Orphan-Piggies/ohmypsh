#!/bin/sh
# Smoke tests for cage, the armory's sandbox.
# Needs Landlock; where the kernel has none, the tests SKIP (cage
# itself refuses to run there, and that refusal is also tested).
# Usage: sh tests/cage.sh   (or: make test)

cd "$(dirname "$0")/.." || exit 1
CAGE=${CAGE:-./cage}
fails=0

check() {
    desc=$1 expected=$2 actual=$3
    if [ "$actual" = "$expected" ]; then
        printf 'ok   %s\n' "$desc"
    else
        printf 'FAIL %s\n     expected: [%s]\n     got:      [%s]\n' \
            "$desc" "$expected" "$actual"
        fails=$((fails + 1))
    fi
}

if ! grep -q landlock /sys/kernel/security/lsm 2>/dev/null; then
    echo "skip cage tests (no Landlock in this kernel's LSM stack)"
    $CAGE -q true 2>/dev/null
    check "without Landlock, cage refuses (125), never runs uncaged" \
        "125" "$?"
    exit 0
fi

tmp=$(mktemp -d)
cd "$tmp" || exit 1
CAGE_BIN=$(cd - > /dev/null && pwd)/${CAGE#./}
cd "$tmp" || exit 1
echo precious > victim.txt

$CAGE_BIN -q sh -c 'echo evil > hacked.txt' 2>/dev/null
check "creating a file outside the cage fails" \
    "no" "$([ -f hacked.txt ] && echo YES || echo no)"

$CAGE_BIN -q sh -c 'echo evil > victim.txt' 2>/dev/null
check "overwriting keeps the original" "precious" "$(cat victim.txt)"

$CAGE_BIN -q rm victim.txt 2>/dev/null
check "deleting is refused" "precious" "$(cat victim.txt)"

check "reading stays open" "precious" "$($CAGE_BIN -q cat victim.txt)"

out=$($CAGE_BIN -q sh -c 'echo loot > "$CAGE_DIR/loot" && cat "$CAGE_DIR/loot"' 2>&1)
check "the scratch dir accepts writes" "loot" "$out"

out=$($CAGE_BIN -q sh -c 'echo "$TMPDIR"' 2>&1)
case $out in
    /tmp/cage-*) printf 'ok   TMPDIR points into the cage\n' ;;
    *) printf 'FAIL TMPDIR is [%s]\n' "$out"; fails=$((fails + 1)) ;;
esac

mkdir okdir
out=$($CAGE_BIN -q -w okdir sh -c 'echo fine > okdir/f && cat okdir/f' 2>&1)
check "-w opens a chosen gate" "fine" "$out"

out=$($CAGE_BIN -q sh -c 'echo x > /dev/null && echo breathes' 2>&1)
check "/dev/null stays writable" "breathes" "$out"

$CAGE_BIN -q sh -c 'exit 7'
check "the child's exit status is forwarded" "7" "$?"

$CAGE_BIN -q definitely-missing-xirt 2>/dev/null
check "a missing command is 127" "127" "$?"

$CAGE_BIN 2>/dev/null
check "no command is a usage error (2)" "2" "$?"

cd / && rm -rf "$tmp"

echo
if [ "$fails" -eq 0 ]; then
    echo "cage holds — nothing got out 🔒"
else
    echo "$fails cage test(s) failed"
    exit 1
fi
