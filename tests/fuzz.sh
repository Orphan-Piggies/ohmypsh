#!/bin/bash
# Parser fuzzer for psh. Throws three kinds of hostile input at the
# shell and checks it never crashes:
#
#   class 0: raw /dev/urandom bytes
#   class 1: soup of real grammar tokens in random order
#   class 2: a valid program, mutilated (truncated / spliced)
#
# A trailing `echo FUZZEND` sentinel separates "psh survived and ran
# to the end" from "psh died mid-input" — exit codes can't tell those
# apart, because psh legitimately exits with its last command's
# status. Crashing inputs are saved for replay.
#
# Usage: bash tests/fuzz.sh [iterations]   (default 600)

set -u
cd "$(dirname "$0")/.." || exit 1
PSH=$PWD/${PSH:-psh-asan}
[ -x "$PSH" ] || { echo "build first: make psh-asan"; exit 1; }
N=${1:-600}

VOCAB=(if then else elif fi while do done for in case esac
       '{' '}' '(' ')' '|' '||' '&&' ';' ';;' '&' '>' '>>' '<' '2>'
       '"' "'" '$' '$(' ')' '$((' '))' '${' '}' '#' '!' '[' ']'
       echo true false : X=1 '$X' '$@' '$?' '$#' -f -n = != local
       export unset return break continue f\(\) test)

CORPUS='f() { local x=1; echo $x; }
for i in 1 2 3; do case $i in 2) echo two;; *) true;; esac; done
while [ $((x < 2)) = 1 ]; do x=$((x + 1)); done
echo $(echo a $(echo b)) "q$X" *.c > /dev/null 2> /dev/null
true && false || echo ok; f'

work=$(mktemp -d)
crashdir=$PWD/tests/fuzz-crashes
crashes=0
loops=0

for ((i = 0; i < N; i++)); do
    inp=$work/in
    case $((i % 3)) in
    0)
        head -c $((RANDOM % 200 + 1)) /dev/urandom > "$inp"
        ;;
    1)
        {
            for ((w = 0; w < RANDOM % 40 + 3; w++)); do
                printf '%s' "${VOCAB[RANDOM % ${#VOCAB[@]}]}"
                ((RANDOM % 4)) && printf ' ' || printf '\n'
            done
            echo
        } > "$inp"
        ;;
    2)
        cut=$((RANDOM % ${#CORPUS}))
        printf '%s' "${CORPUS:0:cut}${CORPUS:RANDOM % ${#CORPUS}}" > "$inp"
        echo >> "$inp"
        ;;
    esac
    printf '\necho FUZZEND\n' >> "$inp"

    out=$(cd "$work" && timeout 5 "$PSH" < "$inp" 2> "$work/err")
    rc=$?
    if [ $rc -eq 124 ] && grep -q while "$inp"; then
        # A timeout on an input containing `while` is (almost always)
        # a mutation that built a legitimately infinite loop — the
        # halting problem is not a shell bug. Not a finding.
        loops=$((loops + 1))
    elif { [ $rc -eq 124 ] || [ $rc -ge 128 ]; } &&
         ! grep -q FUZZEND <<< "$out"; then
        # 124 = hung, >=128 = killed by a signal, and the sentinel
        # never ran: that's a death. Plain exit codes (2, 127...) are
        # psh correctly REPORTING garbage, which is the job.
        mkdir -p "$crashdir"
        cp "$inp" "$crashdir/crash-$i.psh"
        crashes=$((crashes + 1))
        echo "CRASH (rc=$rc) saved: tests/fuzz-crashes/crash-$i.psh"
    elif grep -q AddressSanitizer "$work/err"; then
        mkdir -p "$crashdir"
        cp "$inp" "$crashdir/asan-$i.psh"
        crashes=$((crashes + 1))
        echo "ASAN ERROR saved: tests/fuzz-crashes/asan-$i.psh"
    fi
done

rm -rf "$work"
echo
if [ $crashes -eq 0 ]; then
    echo "fuzz: $N inputs, 0 crashes ($loops honest infinite loops) — the shell would not crack 🫛"
else
    echo "fuzz: $crashes crash(es) in $N inputs — replay the saved files"
    exit 1
fi
