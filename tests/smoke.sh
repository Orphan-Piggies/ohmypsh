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

# ---- milestone 2: pipes, redirects, ';' ----

tmp=$(mktemp -d)

out=$(printf 'echo hello | tr a-z A-Z\n' | $PSH)
check "two-stage pipeline" "HELLO" "$out"

out=$(printf '%s\n' 'printf "c\na\nb\n" | sort | head -1' | $PSH)
check "three-stage pipeline" "a" "$out"

out=$(printf 'echo hi|tr h H\n' | $PSH)
check "pipe without spaces" "Hi" "$out"

out=$(printf 'echo "a|b"\n' | $PSH)
check "quoted pipe is literal" "a|b" "$out"

out=$(printf 'echo a; echo b\n' | $PSH)
check "semicolon runs both commands" "a
b" "$out"

out=$(printf 'echo salted > %s/f\ncat < %s/f\n' "$tmp" "$tmp" | $PSH)
check "output redirect then input redirect" "salted" "$out"

printf 'echo one > %s/g\necho two >> %s/g\n' "$tmp" "$tmp" | $PSH
check "append redirect" "one
two" "$(cat "$tmp/g")"

printf 'ls definitely-missing-xirt 2> %s/err\n' "$tmp" | $PSH
[ -s "$tmp/err" ] && printf 'ok   2> captures stderr\n' \
    || { printf 'FAIL 2> captured nothing\n'; fails=$((fails + 1)); }

printf 'cd /\npwd > %s/p\n' "$tmp" | $PSH
check "lone builtin with redirect (runs in-parent)" "/" "$(cat "$tmp/p")"

printf 'true | false\n' | $PSH
check "pipeline status is the last stage's" "1" "$?"

printf 'false | true\n' | $PSH
check "early-stage failure doesn't set status" "0" "$?"

printf 'ls |\n' | $PSH 2>/dev/null
check "dangling pipe is a syntax error" "2" "$?"

printf 'echo >\n' | $PSH 2>/dev/null
check "redirect without filename is a syntax error" "2" "$?"

out=$(printf 'cd /tmp | cat\npwd\n' | $PSH)
[ "$out" != "/tmp" ] && printf 'ok   builtin in a pipeline runs in a subshell\n' \
    || { printf 'FAIL cd leaked out of a pipeline\n'; fails=$((fails + 1)); }

rm -rf "$tmp"

echo
if [ "$fails" -eq 0 ]; then
    echo "all smoke tests passed 🫛"
else
    echo "$fails test(s) failed"
    exit 1
fi
