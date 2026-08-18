#!/bin/sh
# Smoke tests for psh milestone 1.
# Runs commands through psh non-interactively and checks the output.
# Usage: sh tests/smoke.sh   (or: make test)

cd "$(dirname "$0")/.." || exit 1
PSH=./psh
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

out=$(printf 'echo hello pistachio\n' | $PSH)
check "runs external command with args" "hello pistachio" "$out"

out=$(printf 'echo "two  spaces kept"\n' | $PSH)
check "double quotes preserve spacing" "two  spaces kept" "$out"

out=$(printf "echo 'single quoted'\n" | $PSH)
check "single quotes group a word" "single quoted" "$out"

out=$(printf 'echo ab"c d"e\n' | $PSH)
check "quotes join inside a word" "abc de" "$out"

out=$(cd /tmp && printf 'pwd\n' | "$OLDPWD"/psh 2>/dev/null || true)
# pwd may resolve symlinks (/tmp vs /private/tmp etc.) — just check non-empty
[ -n "$out" ] && printf 'ok   pwd prints a directory\n' \
    || { printf 'FAIL pwd printed nothing\n'; fails=$((fails + 1)); }

printf 'true\n' | $PSH
check "exit status of last command: true" "0" "$?"

printf 'false\n' | $PSH
check "exit status of last command: false" "1" "$?"

printf 'definitely-not-a-command-xirt\n' | $PSH 2>/dev/null
check "command not found exits 127" "127" "$?"

out=$(printf 'cd /\npwd\n' | $PSH)
check "cd persists across lines (builtin runs in-process)" "/" "$out"

printf 'echo "unterminated\n' | $PSH 2>/dev/null
check "unterminated quote is a syntax error (status 2)" "2" "$?"

echo
if [ "$fails" -eq 0 ]; then
    echo "all smoke tests passed 🫛"
else
    echo "$fails test(s) failed"
    exit 1
fi
