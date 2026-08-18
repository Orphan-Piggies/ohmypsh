#!/bin/sh
# Keystroke tests for the cockpit (editor.c): drives psh through a
# real pty via script(1), feeding paced raw bytes — arrows, control
# keys, UTF-8 — and checks what the executed commands printed.
# Usage: sh tests/editor.sh   (or: make test-editor)
# Not part of `make test` yet: pty timing can be jittery on loaded CI.

cd "$(dirname "$0")/.." || exit 1
PSH=${PSH:-./psh}
fails=0

check() {
    desc=$1
    if [ "$2" = "yes" ]; then
        printf 'ok   %s\n' "$desc"
    else
        printf 'FAIL %s\n' "$desc"
        fails=$((fails + 1))
    fi
}

# Feed each input line as raw bytes (\x.. escapes via printf %b),
# paced so the editor sees real keystrokes, not one big paste.
out=$(
  while IFS= read -r chunk; do
      printf '%b' "$chunk"
      sleep 0.2
  done <<'EOF' | PSH_EDITOR=nut script -qec "$PSH" /dev/null | tr -d '\r'
echo plain works\n
echo XY\0033[D\0033[DAB\n
echo abcQ\0177\n
echo əl\0033[D\0033[Dx\n
echo one two\0027\n
echo AABB\0001\0033[C\0033[C\0033[C\0033[C\0033[C\0033[C\0013\n
\0033[A\n
echo end\0002\0002\0002!\n
exit\n
EOF
)

has() { printf '%s\n' "$out" | grep -a "$1" >/dev/null && echo yes; }

check "plain line executes"          "$(has '^plain works$')"
check "arrow-left + insert"          "$(has '^ABXY$')"
check "backspace deletes"            "$(has '^abc$')"
check "utf-8 cursor steps over ə"    "$(has '^xəl$')"
check "ctrl-w kills a word"          "$(has '^one$')"
check "ctrl-a + ctrl-k"              "$(has '^A$')"
n=$(printf '%s\n' "$out" | grep -ac '^A$')
check "up-arrow recalls history"     "$([ "$n" = 2 ] && echo yes)"
check "ctrl-b moves the cursor"      "$(has '^!end$')"

[ "$fails" = 0 ] && printf 'all editor tests passed 🫛\n'
exit "$fails"
