#!/bin/sh
# Smoke tests for psh milestone 1.
# Runs commands through psh non-interactively and checks the output.
# Usage: sh tests/smoke.sh   (or: make test)

cd "$(dirname "$0")/.." || exit 1
PSH=${PSH:-./psh}   # override to test another build: PSH=./psh-asan
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

# ---- milestone 3: && ||, variables, $?, globbing, tilde ----

out=$(printf 'true && echo yes\n' | $PSH)
check "&& runs on success" "yes" "$out"

out=$(printf 'false && echo no\ntrue\n' | $PSH)
check "&& skips on failure" "" "$out"

out=$(printf 'false || echo rescued\n' | $PSH)
check "|| runs on failure" "rescued" "$out"

out=$(printf '%s\n' 'false && echo a || echo b' | $PSH)
check "false && a || b falls through to b" "b" "$out"

out=$(printf 'X=5; echo $X\n' | $PSH)
check "assignment then expansion, same line" "5" "$out"

out=$(printf 'GREETING=salam\necho $GREETING\n' | $PSH)
check "assignment persists across lines" "salam" "$out"

out=$(printf '%s\n' 'A="two  words"; printf [%s]\n $A' | $PSH)
check "founding rule: variables never word-split" "[two  words]" "$out"

out=$(printf '%s\n' 'false; echo $?' | $PSH)
check "\$? expands to last status" "1" "$out"

out=$(printf "echo '\$HOME'\n" | $PSH)
check "single quotes suppress expansion" '$HOME' "$out"

out=$(printf 'V=nut; echo "${V}cracker"\n' | $PSH)
check "braced \${V} expansion inside quotes" "nutcracker" "$out"

out=$(printf 'A=1 printenv A\n' | $PSH)
check "per-command env: A=1 cmd sees A" "1" "$out"

out=$(printf 'A=1 true\nprintenv A\ntrue\n' | $PSH)
check "per-command env does not leak into the shell" "" "$out"

printf 'echo n1 > %s/a.nut\necho n2 > %s/b.nut\n' "$tmp" "$tmp" | $PSH
out=$(printf 'cd %s; echo *.nut\n' "$tmp" | $PSH)
check "glob expands to sorted matches" "a.nut b.nut" "$out"

out=$(printf 'cd %s; echo *.zzz\n' "$tmp" | $PSH)
check "glob with no match stays literal" "*.zzz" "$out"

out=$(printf 'cd %s; echo "*.nut"\n' "$tmp" | $PSH)
check "quoted glob stays literal" "*.nut" "$out"

out=$(printf 'echo ~\n' | $PSH)
check "tilde expands to HOME" "$HOME" "$out"

out=$(printf 'F=%s/var.out; echo piped > $F; cat $F\n' "$tmp" | $PSH)
check "redirect path with a variable" "piped" "$out"

out=$(printf 'ls $NO_SUCH_VAR_SET %s/a.nut\n' "$tmp" | $PSH)
check "empty unquoted expansion vanishes" "$tmp/a.nut" "$out"

# ---- milestone 4: background jobs, wait, comments ----

start=$(date +%s)
out=$(printf 'sleep 2 > /dev/null 2> /dev/null &\necho immediate\n' | $PSH)
elapsed=$(( $(date +%s) - start ))
check "& backgrounds a pipeline" "immediate" "$out"
if [ "$elapsed" -lt 2 ]; then
    printf 'ok   & returns without waiting\n'
else
    printf 'FAIL & blocked for %ss\n' "$elapsed"
    fails=$((fails + 1))
fi

out=$(printf 'sleep 0.2 && echo late > %s/bg.out &\nwait\ncat %s/bg.out\n' \
    "$tmp" "$tmp" | $PSH)
check "background &&-list runs in a subshell; wait blocks" "late" "$out"

printf 'true &\nwait\n' | $PSH
check "wait exits 0" "0" "$?"

out=$(printf '# a full comment line\necho visible\n' | $PSH)
check "comment lines are ignored" "visible" "$out"

out=$(printf 'echo tail # trailing comment\n' | $PSH)
check "trailing comments are stripped" "tail" "$out"

out=$(printf 'echo a#b\n' | $PSH)
check "# inside a word is literal" "a#b" "$out"

printf '& echo x\n' | $PSH 2>/dev/null
check "leading '&' is a syntax error" "2" "$?"

printf 'fg\n' | $PSH 2>/dev/null
check "fg without job control fails politely" "1" "$?"

# ---- milestone 5: the language ----

out=$(printf 'if true; then echo yes; else echo no; fi\n' | $PSH)
check "if / then" "yes" "$out"

out=$(printf 'if false; then echo yes; else echo no; fi\n' | $PSH)
check "if / else" "no" "$out"

out=$(printf 'if false; then echo a; elif true; then echo b; else echo c; fi\n' | $PSH)
check "elif" "b" "$out"

out=$(printf 'if true\nthen\necho multi\nfi\n' | $PSH)
check "multi-line if (accumulated input)" "multi" "$out"

out=$(printf 'N=0; while [ $N != 3 ]; do echo $N; N=$(expr $N + 1); done\n' | $PSH)
check "while loop with \$( ) in the step" "0
1
2" "$out"

out=$(printf 'for x in a b c; do echo x$x; done\n' | $PSH)
check "for loop" "xa
xb
xc" "$out"

out=$(printf '%s\n' 'for i in 1 2 3 4; do if [ $i = 3 ]; then break; fi; echo $i; done' | $PSH)
check "break" "1
2" "$out"

out=$(printf '%s\n' 'for i in 1 2 3; do if [ $i = 2 ]; then continue; fi; echo $i; done' | $PSH)
check "continue" "1
3" "$out"

out=$(printf 'greet() { echo hello $1; }\ngreet world\n' | $PSH)
check "function definition and call with \$1" "hello world" "$out"

out=$(printf 'f() { return 3; }\nf\necho $?\n' | $PSH)
check "return sets the function's status" "3" "$out"

out=$(printf 'f() { echo $#; }\nf a b c\n' | $PSH)
check "\$# inside a function" "3" "$out"

out=$(printf 'echo $(echo nested)\n' | $PSH)
check "command substitution" "nested" "$out"

out=$(printf 'D=$(pwd); echo $D\n' | $PSH)
check "cmdsub into a variable" "$(pwd)" "$out"

out=$(printf '%s\n' 'for f in $(printf "a\nb"); do echo x$f; done' | $PSH)
check "unquoted cmdsub splits on newlines" "xa
xb" "$out"

out=$(printf '%s\n' 'echo "$(printf "a\nb")" | wc -l' | $PSH)
check "quoted cmdsub stays one word (2 lines)" "2" "$out"

out=$(printf '%s\n' 'echo $(printf "a\nb") | wc -l' | $PSH)
check "unquoted cmdsub joins to one line" "1" "$out"

printf '#!/usr/bin/env psh\necho script-ran $1\n' > "$tmp/s.psh"
out=$($PSH "$tmp/s.psh" world)
check "script file with shebang and \$1" "script-ran world" "$out"

out=$($PSH -c 'echo dash-c-works')
check "psh -c" "dash-c-works" "$out"

printf 'SRCVAR=from-source\n' > "$tmp/lib.psh"
out=$(printf 'source %s/lib.psh\necho $SRCVAR\n' "$tmp" | $PSH)
check "source runs in the current shell" "from-source" "$out"

printf 'if true; then\n' | $PSH 2>/dev/null
check "EOF inside a construct is an error non-interactively" "2" "$?"

out=$(printf 'twice() { echo $1$1; }\ntwice nut | tr a-z A-Z\n' | $PSH)
check "function inside a pipeline" "NUTNUT" "$out"

# ---- H1: variable table, local, export, $@, case, arithmetic ----

out=$(printf 'X=5; printenv X\ntrue\n' | $PSH)
check "plain assignment is NOT exported (the leak is sealed)" "" "$out"

out=$(printf 'X=5; export X; printenv X\n' | $PSH)
check "export promotes to the environment" "5" "$out"

out=$(printf 'export Y=7; printenv Y\n' | $PSH)
check "export NAME=value" "7" "$out"

out=$(PATH_TEST=inherited $PSH -c 'PATH_TEST=updated; printenv PATH_TEST')
check "inherited env names stay exported on assignment" "updated" "$out"

out=$(printf 'f() { local X=inner; echo $X; }\nX=outer\nf\necho $X\n' | $PSH)
check "local shadows, then restores" "inner
outer" "$out"

out=$(printf 'f() { local X=only; }\nf\necho [$X]\n' | $PSH)
check "locals die at function return" "[]" "$out"

out=$(printf 'g() { echo $V; }\nf() { local V=dyn; g; }\nf\n' | $PSH)
check "dynamic scoping: callee sees caller's local" "dyn" "$out"

out=$(printf 'X=5; unset X; echo [$X]\n' | $PSH)
check "unset removes a variable" "[]" "$out"

out=$(printf 'f() { printf "[%%s]" $@; echo; }\nf a "b c" d\n' | $PSH)
check "\$@ splat keeps arguments whole" "[a][b c][d]" "$out"

out=$(printf 'g() { echo $#; }\nf() { g $@; }\nf a b c\n' | $PSH)
check "\$@ forwards argument count intact" "3" "$out"

out=$(printf 'case nut.c in *.psh) echo shell;; *.c) echo cfile;; *) echo other;; esac\n' | $PSH)
check "case matches a glob pattern" "cfile" "$out"

out=$(printf 'case hello in a|hello|b) echo alt;; esac\n' | $PSH)
check "case pattern alternation with |" "alt" "$out"

out=$(printf 'case zzz in *.c) echo no;; esac\necho after $?\n' | $PSH)
check "case with no match: status 0, life goes on" "after 0" "$out"

out=$(printf 'echo $((2 + 3 * 4))\n' | $PSH)
check "arithmetic precedence" "14" "$out"

out=$(printf 'N=5; echo $((N * 2 - 1))\n' | $PSH)
check "arithmetic reads variables bare" "9" "$out"

out=$(printf 'N=0\nwhile [ $((N < 3)) = 1 ]; do echo $N; N=$((N + 1)); done\n' | $PSH)
check "arithmetic loop (expr retired)" "0
1
2" "$out"

out=$(printf 'echo $((10 / 0))\n' | $PSH 2>/dev/null; echo x$?)
check "division by zero cracks loudly, not fatally" "
x0" "$out"

# ---- H2: test/[ builtins, type, command -v, set -e, trap, lines ----

out=$(printf 'type [\n' | $PSH)
check "[ is a builtin now (the fork is gone)" "[ is a shell builtin" "$out"

out=$(printf '[ -d src -a -f Makefile ] && echo both\n' | $PSH)
check "test: file ops with -a" "both" "$out"

out=$(printf '[ 5 -lt 10 ] && [ abc != abd ] && echo logic\n' | $PSH)
check "test: numeric and string comparisons" "logic" "$out"

out=$(printf '[ ! -f no-such-file-xirt ] && echo negated\n' | $PSH)
check "test: ! negation" "negated" "$out"

out=$(printf '[ -z "" ] && [ -n x ] && echo strings\n' | $PSH)
check "test: -z and -n" "strings" "$out"

printf '[ 5 -lt 10\n' | $PSH 2>/dev/null
check "[ without ] is an error (status 2)" "2" "$?"

printf '[ 5 -lt banana ]\n' | $PSH 2>/dev/null
check "test: non-numeric with -lt is an error" "2" "$?"

out=$(printf 'type cd\n' | $PSH)
check "type: builtin" "cd is a shell builtin" "$out"

out=$(printf 'f() { true; }\ntype f\n' | $PSH)
check "type: function" "f is a function" "$out"

printf 'type definitely-not-installed-xirt\n' | $PSH 2>/dev/null
check "type: not found sets status 1" "1" "$?"

out=$(printf 'command -v cd\n' | $PSH)
check "command -v: builtin prints its name" "cd" "$out"

out=$(printf 'command -v ls > /dev/null && echo found\n' | $PSH)
check "command -v: PATH lookup succeeds silently" "found" "$out"

out=$(printf 'command -v no-such-xirt || echo missing\n' | $PSH)
check "command -v: missing is silent, status 1" "missing" "$out"

out=$(printf 'set -e\nfalse\necho unreachable\n' | $PSH)
check "set -e stops at the first failure" "" "$out"

printf 'set -e\nfalse\necho x\n' | $PSH 2>/dev/null
check "set -e exits with the failing status" "1" "$?"

out=$(printf 'set -e\nfalse || true\nif false; then true; fi\necho survived\n' | $PSH)
check "set -e ignores tested failures (|| and if)" "survived" "$out"

out=$(printf 'trap "echo bye" EXIT\necho hi\n' | $PSH)
check "trap EXIT fires at end of input" "hi
bye" "$out"

printf 'trap "echo t" EXIT\nexit 3\n' | $PSH > /dev/null
check "trap preserves the exit status" "3" "$?"

out=$(printf 'trap "echo no" EXIT\ntrap - EXIT\necho only\n' | $PSH)
check "trap - EXIT clears the trap" "only" "$out"

printf 'echo one\necho two\necho )\n' > "$tmp/bad.psh"
err=$($PSH "$tmp/bad.psh" 2>&1 >/dev/null)
case "$err" in
    *"line 3"*) printf 'ok   script parse errors carry line numbers\n' ;;
    *) printf 'FAIL expected "line 3" in: %s\n' "$err"; fails=$((fails + 1)) ;;
esac

# ---- H3: the omp plugin fleet ----

OMPD=$PWD/omp

out=$(printf 'venv-init() { echo hyphens; }\nvenv-init\n' | $PSH)
check "hyphenated function names parse (plugin authors rejoice)" "hyphens" "$out"

mkdir -p "$tmp/proj/.venv/bin" "$tmp/proj/deep/nest"
touch "$tmp/proj/.venv/bin/activate"
out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=python\nsource $OMP_DIR/omp.psh\ncd %s/proj\nomp_precmd\necho A=$(basename $(dirname $VIRTUAL_ENV))\n' "$OMPD" "$tmp" | $PSH)
check "venv auto-activates on cd into a project" "🫛 venv: proj activated
A=proj" "$out"

out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=python\nsource $OMP_DIR/omp.psh\ncd %s/proj/deep/nest\nomp_precmd\necho A=$(basename $(dirname $VIRTUAL_ENV))\ncd /\nomp_precmd\necho B=[$VIRTUAL_ENV]\n' "$OMPD" "$tmp" | $PSH | grep -v 'venv:')
check "venv found from deep subdirs; deactivates on leaving" "A=proj
🫛 venv deactivated
B=[]" "$out"

out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS="python shell-shock"\nsource $OMP_DIR/omp.psh\nfalse\nomp_precmd\n' "$OMPD" | $PSH)
check "hook dispatcher: XIRT rides the shared precmd" "🫛 XIRT! (exit 1)" "$out"

out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=git\nsource $OMP_DIR/omp.psh\nomp enable django > /dev/null\ntype dj\n' "$OMPD" | $PSH)
check "omp enable loads a plugin live" "dj is a function" "$out"

out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=basics\nsource $OMP_DIR/omp.psh\nomp list\n' "$OMPD" | $PSH | wc -l)
if [ "$out" -ge 12 ]; then
    printf 'ok   omp list inventories the whole fleet (%s lines)\n' "$out"
else
    printf 'FAIL omp list printed only %s lines\n' "$out"
    fails=$((fails + 1))
fi

out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=git\nsource $OMP_DIR/omp.psh\nomp theme plain > /dev/null\necho $PSH_PROMPT\n' "$OMPD" | $PSH)
check "omp theme switches the prompt template" 'psh$(__omp_status) $ ' "$out"

# ---- the crunchy fleet: z, extract, duration ----

mkdir -p "$tmp/deep/nest-alpha" "$tmp/other"
out=$(printf 'OMP_DIR=%s\n__OMP_Z_FILE=%s/zdata\nOMP_PLUGINS=z\nsource $OMP_DIR/omp.psh\ncd %s/deep/nest-alpha\nomp_precmd\ncd %s/other\nomp_precmd\ncd /\nomp_precmd\nz alpha\npwd\n' \
    "$OMPD" "$tmp" "$tmp" "$tmp" | $PSH | tail -1)
check "z jumps to the best frecency match" "$tmp/deep/nest-alpha" "$out"

out=$(printf 'OMP_DIR=%s\n__OMP_Z_FILE=%s/zdata\nOMP_PLUGINS=z\nsource $OMP_DIR/omp.psh\nz nosuchplace-qq\n' \
    "$OMPD" "$tmp" | $PSH)
check "z misses politely" "z: no match for nosuchplace-qq in the bag" "$out"

mkdir -p "$tmp/arch" && echo "kernel of truth" > "$tmp/arch/nut.txt"
tar czf "$tmp/arch/nut.tar.gz" -C "$tmp/arch" nut.txt && rm "$tmp/arch/nut.txt"
out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=extract\nsource $OMP_DIR/omp.psh\ncd %s/arch\nextract nut.tar.gz\ncat nut.txt\n' \
    "$OMPD" "$tmp" | $PSH)
check "extract cracks a tar.gz" "kernel of truth" "$out"

out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=extract\nsource $OMP_DIR/omp.psh\nextract mystery.nut\n' "$OMPD" | $PSH)
check "extract rejects unknown shells" "extract: mystery.nut: no such file" "$out"

out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=duration\nsource $OMP_DIR/omp.psh\nPSH_CMD_MS=3400\n__omp_duration_prompt\n' "$OMPD" | $PSH)
case $out in *"3.4s"*) out=3.4s ;; esac
check "duration formats seconds" "3.4s" "$out"

out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=duration\nsource $OMP_DIR/omp.psh\nPSH_CMD_MS=83000\n__omp_duration_prompt\n' "$OMPD" | $PSH)
case $out in *"1m23s"*) out=1m23s ;; esac
check "duration formats minutes" "1m23s" "$out"

out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=duration\nsource $OMP_DIR/omp.psh\nPSH_CMD_MS=900\n__omp_duration_prompt\necho quiet=$?\n' "$OMPD" | $PSH)
check "duration stays quiet under the threshold" "quiet=0" "$out"

# ---- omp redis plugin (pure psh on the H7 wire) ----

out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=redis\nsource $OMP_DIR/omp.psh\nOMP_REDIS_PORT=1\nrping\necho down=$?\n' "$OMPD" | $PSH 2>/dev/null)
check "redis plugin: rping with no server fails politely" "down=1" "$out"

out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=redis\nsource $OMP_DIR/omp.psh\nOMP_REDIS_PORT=1\n__omp_redis_prompt\necho quiet=$?\necho err-ok >&2\n' "$OMPD" | $PSH 2>&1)
check "redis plugin: prompt segment is silent when down, stderr intact" "quiet=0
err-ok" "$out"

# live tests, only when something PONGs on the default port
if printf 'OMP_DIR=%s\nOMP_PLUGINS=redis\nsource $OMP_DIR/omp.psh\nrping\n' "$OMPD" | $PSH 2>/dev/null | grep -q PONG; then
    out=$(printf 'OMP_DIR=%s\nOMP_PLUGINS=redis\nsource $OMP_DIR/omp.psh\nrset __omp_smoke "two words" > /dev/null\nrget __omp_smoke\nrdel __omp_smoke > /dev/null\nrget __omp_smoke\necho nil=$?\n' "$OMPD" | $PSH 2>/dev/null)
    check "redis plugin: set/get/del round-trip (live server)" "two words
nil=1" "$out"
else
    printf 'skip redis live round-trip (nothing on 6379)\n'
fi

# ---- H9.1: ${...} parameter operators ----
# (patterns passed as printf ARGUMENTS — %% in a format string lies)

out=$(printf '%s\n' 'p=abc.tar.gz; echo ${p%.*} ${p%%.*} ${p#*.} ${p##*.}' | $PSH)
check "affix strips: % %% # ##" "abc.tar abc tar.gz gz" "$out"

out=$(printf '%s\n' 'x="a b c"; echo "[${x%% *}]" "[${x// /-}]" "[${x/ /_}]"' | $PSH)
check "patterns with spaces lex as one word; / and // replace" \
    "[a] [a-b-c] [a_b c]" "$out"

out=$(printf '%s\n' 'x=pistachio; echo ${#x} ${#missing}' | $PSH)
check "length: bytes, unset is 0" "9 0" "$out"

out=$(printf '%s\n' 'x=abc.tar.gz; e=.gz; echo ${x%$e}' | $PSH)
check "the operator pattern expands (\$e inside)" "abc.tar" "$out"

out=$(printf '%s\n' 'echo "[${missing%.x}]"' | $PSH)
check "operators on unset act on empty" "[]" "$out"

out=$(printf '%s\n' 'f() { echo ${10}; }; f 1 2 3 4 5 6 7 8 9 ten' | $PSH)
check "multi-digit positionals: \${10}" "ten" "$out"

out=$(printf '%s\n' 'x=1; echo "[${x@}]"' | $PSH 2>/dev/null)
check "unknown form expands empty…" "[]" "$out"
n=$(printf '%s\n' 'x=1; echo ${x@}' | $PSH 2>&1 >/dev/null | grep -c "bad substitution")
check "…but complains exactly ONCE on stderr" "1" "$n"

# the audit's companion bug: external commands expanded argv twice,
# running $( ) substitutions (and their side effects) twice
: > "$tmp/ticks"
printf '/bin/echo hi $(echo tick >> %s/ticks) > /dev/null\n' "$tmp" | $PSH
check "cmdsub in an external command's argv runs ONCE" \
    "1" "$(grep -c tick "$tmp/ticks")"

# ---- H7.1: numbered fds, dup/close, <>, and ordering ----

out=$(printf 'ls /definitely-missing-xirt 2>&1 | grep -c xirt\n' | $PSH)
check "2>&1 folds stderr into the pipe" "1" "$out"

printf 'ls /definitely-missing-xirt > %s/both 2>&1\n' "$tmp" | $PSH
out=$(grep -c xirt "$tmp/both")
check ">f 2>&1 lands stderr in the file" "1" "$out"

# swapped order: 2 dups the OLD stdout, then 1 moves to the file —
# so the error stays on the script's stdout and the file gets nothing
out=$(printf 'ls /definitely-missing-xirt 2>&1 > %s/oo\n' "$tmp" | $PSH 2>/dev/null | grep -c xirt)
check "2>&1 >f applies in source order" "1" "$out"
check "2>&1 >f leaves the file empty" "0" "$(wc -c < "$tmp/oo" | tr -d ' ')"

printf 'echo oops >&2\n' | $PSH 2>"$tmp/e" >"$tmp/o"
check "builtin echo >&2 reaches stderr" "oops" "$(cat "$tmp/e")"
check "builtin echo >&2 skips stdout" "0" "$(wc -c < "$tmp/o" | tr -d ' ')"

out=$(printf 'ls /definitely-missing-xirt 2>&-\necho done\n' | $PSH 2>/dev/null)
check "2>&- closes stderr, shell survives" "done" "$out"

printf 'ls /nope-a 2>> %s/e2\nls /nope-b 2>> %s/e2\n' "$tmp" "$tmp" | $PSH
check "numbered append 2>> accumulates" "2" "$(grep -c nope "$tmp/e2")"

printf 'echo salam 1<> %s/rw\n' "$tmp" | $PSH
check "read-write <> creates and writes" "salam" "$(cat "$tmp/rw")"

out=$(printf 'echo 2 > %s/two\ncat %s/two\n' "$tmp" "$tmp" | $PSH)
check "unglued digit stays an argument (IO_NUMBER)" "2" "$out"

printf 'echo glued 2> %s/eg\n' "$tmp" | $PSH > "$tmp/go"
check "glued 2> redirects fd 2, stdout unharmed" "glued" "$(cat "$tmp/go")"
check "glued 2> leaves the file empty" "0" "$(wc -c < "$tmp/eg" | tr -d ' ')"

out=$(printf 'FD=1\nls /definitely-missing-xirt 2>&$FD\n' | $PSH 2>/dev/null | grep -c xirt)
check "dup target can be a variable (2>&\$FD)" "1" "$out"

printf 'echo hi >&7\n' | $PSH 2>/dev/null
check "dup of a never-opened fd fails" "1" "$?"

printf 'pwd 3> %s/fd3\necho after=$?\n' "$tmp" | $PSH > /dev/null
check "in-parent builtin with fd 3 redirect survives" "0" "$?"

printf 'echo x >&\n' | $PSH 2>/dev/null
check "dangling >& is a syntax error" "2" "$?"

# ---- H7.2: the exec builtin ----

out=$(printf 'exec 3> %s/fd3\necho first >&3\necho second >&3\nexec 3>&-\ncat %s/fd3\n' "$tmp" "$tmp" | $PSH)
check "exec 3>f persists across commands, 3>&- closes" "first
second" "$out"

printf 'exec > %s/exo\necho routed\n' "$tmp" | $PSH
check "exec >f rewires the shell's stdout for good" "routed" "$(cat "$tmp/exo")"

out=$(printf 'exec echo replaced\necho never-printed\n' | $PSH)
check "exec CMD becomes the command (script ends)" "replaced" "$out"

out=$(printf 'exec definitely-missing-xirt\necho alive=$?\n' | $PSH 2>/dev/null)
check "failed exec CMD: shell survives with 127" "alive=127" "$out"

out=$(printf 'exec < /definitely-missing-xirt\necho alive=$?\n' | $PSH 2>/dev/null)
check "failed exec redirect: shell survives" "alive=1" "$out"

out=$(printf 'echo hi | exec tr h H\n' | $PSH)
check "exec in a pipeline replaces the child" "Hi" "$out"

out=$(printf 'exec\necho bare=$?\n' | $PSH)
check "bare exec is a status-0 no-op" "bare=0" "$out"

out=$(printf 'type exec\n' | $PSH)
check "type knows exec" "exec is a shell builtin" "$out"

# the H7 dress rehearsal: a redis-shaped session against a plain file
out=$(printf 'exec 3<> %s/wire\necho PING >&3\nexec 3>&-\nexec 4< %s/wire\nhead -n 1 <&4\n' "$tmp" "$tmp" | $PSH)
check "open 3<>, write >&3, reopen, read <&4 — the wire holds" "PING" "$out"

# ---- H7.3: /dev/tcp — sockets behind open() ----

out=$(printf 'exec 3<> /dev/tcp/127.0.0.1/1\necho alive=$?\n' | $PSH 2>/dev/null)
check "refused connection is an error, not an exit" "alive=1" "$out"

out=$(printf 'cat < /dev/tcp/only-host\necho alive=$?\n' | $PSH 2>/dev/null)
check "malformed /dev/tcp path is an error" "alive=1" "$out"

printf 'echo ping > /dev/udp/127.0.0.1/9\n' | $PSH 2>/dev/null
check "udp send to the discard port" "0" "$?"

if command -v python3 >/dev/null 2>&1; then
    python3 -c '
import socketserver
class Echo(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            self.wfile.write(line); self.wfile.flush()
srv = socketserver.TCPServer(("127.0.0.1", 0), Echo)
print(srv.server_address[1], flush=True)
srv.serve_forever()
' > "$tmp/port" 2>/dev/null &
    NETPID=$!
    i=0
    while [ ! -s "$tmp/port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    out=$(printf 'exec 3<> /dev/tcp/127.0.0.1/%s\necho salam >&3\nhead -n 1 <&3\n' "$(cat "$tmp/port")" | $PSH)
    check "tcp roundtrip: exec 3<>/dev/tcp, write >&3, read <&3" "salam" "$out"

    # the H7.4 proof: TWO lines sent before any read — head would
    # gulp both in one buffer; byte-at-a-time read takes exactly one
    out=$(printf 'exec 3<> /dev/tcp/127.0.0.1/%s\necho first >&3\necho second >&3\nread -r a <&3\nread -r b <&3\necho "$a/$b"\nexec 3>&-\n' "$(cat "$tmp/port")" | $PSH)
    check "read -r takes one reply per call from a burst" "first/second" "$out"
    kill $NETPID 2>/dev/null
else
    printf 'skip tcp roundtrip (no python3)\n'
fi

# ---- H7.4: the read builtin ----

printf '  spaced  out  \n' > "$tmp/rl"
out=$(printf 'read x < %s\necho "[$x]"\n' "$tmp/rl" | $PSH)
check "read with ONE name takes the line verbatim" "[  spaced  out  ]" "$out"

printf 'a b c d\n' > "$tmp/rl"
out=$(printf 'read x y < %s\necho "$x/$y"\n' "$tmp/rl" | $PSH)
check "several names split on blanks, last takes the rest" "a/b c d" "$out"

printf 'crlf\r\n' > "$tmp/rl"
out=$(printf 'read -r x < %s\nif [ "$x" = crlf ]; then echo clean; fi\n' "$tmp/rl" | $PSH)
check "trailing CR of CRLF is stripped" "clean" "$out"

printf 'one\\\ntwo\n' > "$tmp/rl"
out=$(printf 'read x < %s\necho "$x"\n' "$tmp/rl" | $PSH)
check "backslash-newline continues the line without -r" "onetwo" "$out"
out=$(printf 'read -r x < %s\necho "$x"\n' "$tmp/rl" | $PSH)
check "-r keeps the backslash raw" "one\\" "$out"

printf 'partial' > "$tmp/rl"
out=$(printf 'read x < %s\necho "$?:$x"\n' "$tmp/rl" | $PSH)
check "EOF: partial line assigned, status 1" "1:partial" "$out"

out=$(printf 'read 1bad < /dev/null\necho status=$?\n' | $PSH 2>/dev/null)
check "read rejects an invalid name (status 2)" "status=2" "$out"

# a function may redefine ITSELF while running (omp reload does this
# via source) — the old body must outlive the call, not be freed
# under the interpreter's feet
printf 'f() { f() { echo new; }\necho old\n}\nf\nf\necho fine=$?\n' | $PSH > "$tmp/redef" 2>/dev/null
check "self-redefining function survives" "old
new
fine=0" "$(cat "$tmp/redef")"

rm -rf "$tmp"

# ---- history builtin (list lives in the editor; empty non-interactively) ----

out=$(printf 'type history\n' | $PSH)
check "history is a builtin" "history is a shell builtin" "$out"

out=$(printf 'history\necho empty=$?\n' | $PSH)
check "history: empty in a script, still succeeds" "empty=0" "$out"

out=$(printf 'history -c\necho cleared=$?\n' | $PSH)
check "history -c succeeds" "cleared=0" "$out"

printf 'history banana\n' | $PSH 2>/dev/null
check "history with a non-count is an error (status 2)" "2" "$?"

echo
if [ "$fails" -eq 0 ]; then
    echo "all smoke tests passed 🫛"
else
    echo "$fails test(s) failed"
    exit 1
fi
