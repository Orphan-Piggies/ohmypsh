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
whoam\011\n
echo unique-fnd42\n
\0022fnd42\n
head -1 READM\011\n
\0033[200~echo pasted\necho again\0033[201~\n
EDITOR="sed -i s/AAA/BBB/"\n
echo AAA\0030\0005\n
echo sugg-target-xyz\n
echo sugg-t\0033[C\n
if true; then true; fi\n
nosuchcmd-qq\n
echo "ystr"\n
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
check "tab completes a command"      "$(has "^$(whoami)\$")"
n=$(printf '%s\n' "$out" | grep -ac '^unique-fnd42$')
check "ctrl-r finds and executes"    "$([ "$n" = 2 ] && echo yes)"
check "tab completes a filename"     "$(has '^# 🫛 psh')"
check "paste lands without running"  "$(has '^pasted$')"
n=$(printf '%s\n' "$out" | grep -ac '^again$')
check "pasted 2nd line runs on ⏎"    "$([ "$n" = 1 ] && echo yes)"
check "ctrl-x ctrl-e edits in EDITOR" "$(has '^BBB$')"
check "autosuggestion painted grey"   "$(has '90marget-xyz')"
n=$(printf '%s\n' "$out" | grep -ac '^sugg-target-xyz$')
check "right-arrow accepts suggestion" "$([ "$n" = 2 ] && echo yes)"
check "valid command painted green"   "$(has '\[32mecho')"
check "unknown command painted red"   "$(has '\[31mnosuchcmd-qq')"
check "keyword painted bold"          "$(has '\[1mif')"
check "string painted yellow"         "$(has '\[33m"ystr"')"

[ "$fails" = 0 ] && printf 'all editor tests passed 🫛\n'
exit "$fails"
