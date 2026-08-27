# 🫛 psh — the pistachio shell

[![CI](https://github.com/marmeladze/ohmypsh/actions/workflows/ci.yml/badge.svg)](https://github.com/marmeladze/ohmypsh/actions/workflows/ci.yml)

> The pistachio has one of the hardest shells in the world. It deserved
> a place in the hall of fame next to bash, fish, and zsh. So here it is.

`psh` is a Unix shell written from scratch in C — a real one, with
`fork()` and `exec()` and everything. It follows the **familiar core**
philosophy: your fingers already know it (`ls | grep foo`, `$HOME`,
`&&`), but the classic sh traps are fixed — most importantly, variables
will never word-split, so `rm $file` deletes exactly one file, always.

## Status

Horizon 4 of [the roadmap](docs/ROADMAP.md) — the cockpit: a
hand-rolled raw-mode line editor with **fish-style autosuggestions**
(your history, grey, one → away), **syntax highlighting straight
from the shell's real lexer** (valid commands green, typos red
before you press Enter, keywords bold, strings yellow), incremental
Ctrl-R search, tab completion, bracketed paste, multi-line editing,
and Ctrl-X Ctrl-E to finish the line in your `$EDITOR`. Zero
dependencies — readline sailed home in v0.11.0.

Underneath: a complete language (`if`/`elif`/`else`, `while`, `for`,
`case`, functions with `$1`..`$9` and a true `$@` splat,
`local`/`export`/`unset` on a real three-tier variable table,
`$((...))` arithmetic, `$(...)` command substitution), hardened for
scripts (`test`/`[` as fork-free builtins, `type`, `command -v`,
`set -e`, `trap ... EXIT`, parse errors with line numbers), on top
of full job control (`&`, Ctrl-Z, `jobs`/`fg`/`bg`/`wait`),
pipelines, redirects, `&&`/`||`, globbing, `~/.pshrc`, scripts with
shebangs, `psh -c`, `source`, and `#` comments.

Two founding rules, both real: a `$VAR` NEVER word-splits
(`F="two words"; rm $F` deletes exactly one file), and `$(...)`
splits on newlines only (`for f in $(ls)` iterates lines, spaces in
filenames survive).

## 🫛 oh-my-psh

The framework this project was named for — written in psh, running
on psh. Themes are prompt templates re-expanded on every prompt;
plugins are sourced files of functions; `omp_precmd` runs before
each prompt. Install it into your `~/.pshrc`:

```sh
./psh omp/install.psh
```

Ships with the **antep** theme (maximum green: directory, ⌁venv,
git branch with ✗ dirty marker and ↑↓ arrows, red failure status)
and a fleet of plugins: **python** (venvs auto-activate when you cd
into a project — never type `source .venv/bin/activate` again),
**git**, **django**, **flask**, **rails**, **docker**, **npm**,
**basics** (`ll`, `la`, `mkcd`), **redis** (`rget`/`rset`/`rmon`, a
real client over `exec 9<>/dev/tcp` — pure psh, no redis-cli), and
**shell-shock**, which — as the
founding documents foretold — goes *XIRT!* when a command fails.
Drive it with `omp list`, `omp enable <plugin>`, `omp theme <t>`.
Plugins register on precmd/chpwd hooks; writing your own is ~20
lines of psh.

Antep Mode in the wild — the git segment catching a tree going
dirty:

![touch a file and the ✗ dirty marker appears](docs/screenshots/dirty-stage.png)

a venv activating itself on `cd` in (and deactivating on `cd` out):

![cd into a project and its venv activates, ⌁ segment appears](docs/screenshots/venv-discovery.png)

and shell-shock keeping morale up after a typo:

![a typo gets a flavor line, XIRT!, and a red 127 status segment](docs/screenshots/xirt.png)

## 🧂 salt

The armory's cat. Prints files like cat, colors them like bat, under
one rule: **the bytes are never changed, only colored.** Copying
salted output yields exact source (no gutters, no frames); piped
output is byte-identical to the input; `tail -f log | salt -l yaml`
highlights live, line by line. Markdown renders headers, emphasis
and links, and fenced code blocks are highlighted in their own
language — while staying valid Markdown on the clipboard.

```sh
salt file.c              # colors, and only colors
salt README.md           # markdown worth reading
tail -f app.log | salt -l yaml    # streams
salt -c big.py | less -R          # pages
```

Languages: C, sh/psh, Python, Markdown, JSON, diff, YAML, TOML,
JS/TS, Go, Rust, Ruby, Elixir. `-n` for line numbers (opt-in — they
do land in copies), `-L` lists the arsenal.

## Build & run

```sh
make        # builds ./psh and ./salt
make test   # runs the smoke tests
./psh       # step into the bag
```

Requires a C compiler and a POSIX system. That's the whole list —
no libraries, no packages, no `-dev` anything.

## Layout

```
src/main.c       the REPL, scripts, -c, ~/.pshrc, multi-line input
src/lexer.c      input → tokens (words raw; quotes and $() intact)
src/parser.c     recursive descent: if/while/for/functions/lists
src/expand.c     $VAR, $1..$9, $@, $(...), $((...)), ~, quotes, glob
src/vars.c       locals → shell vars → environ; export / local / unset
src/arith.c      the $(( ... )) evaluator
src/testcmd.c    the test / [ builtin (fork-free conditions)
src/exec.c       tree walker: pipelines, control flow, functions
src/jobs.c       job control: process groups, tcsetpgrp, Ctrl-Z
src/editor.c     the cockpit: autosuggestions, syntax highlighting,
                 ^R search, tab completion, ^X^E, bracketed paste
src/complete.c   completion candidates (commands + files)
src/builtins.c   cd, exit, pwd, help, …
src/pistachio.c  🫛 easter pistachios live here, and only here
tools/salt.c     🧂 salt: cat with colors, faithful to the bytes
omp/             oh-my-psh: themes/, plugins/, install.psh — in psh
docs/ROADMAP.md  where this is going
extras/lore/     the sacred founding documents (oh-my-pistachio era)
```

## Salt levels

Some behavior in this shell is nutritionally unnecessary. It is all
contained in `src/pistachio.c` and guaranteed never to affect
correctness. Try `help` and read closely.

## Quality

`make test` (116 smoke tests), `make test-asan` (the same suite under
AddressSanitizer + LeakSanitizer — zero errors, zero leaks), and
`bash tests/fuzz.sh` (2000 hostile inputs, zero crashes: the shell
would not crack). CI runs all three on every push. `man docs/psh.1`
for the manual.

## License

MIT (see LICENSE) — source and binaries alike, now that the last
GPL-licensed link went overboard.

---

*NTT layihəsidir. Sistem indi duzludur.* 🫛
