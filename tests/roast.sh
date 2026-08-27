#!/bin/sh
# Smoke tests for roast, the armory's Markdown renderer.
# Piped output has no color, so these check the pure transforms —
# the glyphs that salt would never dare emit.
# Usage: sh tests/roast.sh   (or: make test)

cd "$(dirname "$0")/.." || exit 1
ROAST=${ROAST:-./roast}
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

out=$(printf '# Hi\n' | $ROAST)
check "h1 drops the # and gains an underline" "Hi
━━" "$out"

out=$(printf '### Deep\n' | $ROAST)
check "h3 drops the #s, no underline" "Deep" "$out"

out=$(printf -- '- [x] shipped\n- [ ] pending\n' | $ROAST)
check "task boxes become glyphs" "☑ shipped
☐ pending" "$out"

out=$(printf -- '- top\n  - nested\n' | $ROAST)
check "bullets become dots, nesting changes the dot" "• top
  ◦ nested" "$out"

out=$(printf 'has **bold** and *ital* and `code`\n' | $ROAST)
check "emphasis markers are consumed" "has bold and ital and code" "$out"

out=$(printf 'snake_case_name stays whole\n' | $ROAST)
check "underscores inside words are not emphasis" \
    "snake_case_name stays whole" "$out"

out=$(printf 'see [docs](http://x)\n' | $ROAST)
check "links render as text (url) when piped" "see docs (http://x)" "$out"

out=$(printf '\\*not bold\\*\n' | $ROAST)
check "escapes drop the backslash, keep the char" "*not bold*" "$out"

out=$(printf '```c\ncode line\n```\n' | $ROAST)
check "fences vanish, code gets a gutter" "  │ code line" "$out"

out=$(printf '> wisdom\n' | $ROAST)
check "blockquote grows a bar" "┃ wisdom" "$out"

out=$(printf -- '---\n' | $ROAST -w 4)
check "hr spans the asked width" "────" "$out"

out=$(printf '| a | b |\n|---|---|\n| 1 | 2 |\n' | $ROAST)
check "tables align with rules" " a │ b
───┼───
 1 │ 2" "$out"

out=$(printf '| not | a table\nplain line\n' | $ROAST)
check "a lone pipe line is not a table" "| not | a table
plain line" "$out"

printf 'x\0y' > /tmp/roast-bin.$$
$ROAST /tmp/roast-bin.$$ >/dev/null 2>&1
check "binary input is refused (status 1)" "1" "$?"
rm -f /tmp/roast-bin.$$

$ROAST /definitely-missing 2>/dev/null
check "missing file is status 1" "1" "$?"

echo
if [ "$fails" -eq 0 ]; then
    echo "roast passes — well done (medium-rare) 🔥"
else
    echo "$fails roast test(s) failed"
    exit 1
fi
