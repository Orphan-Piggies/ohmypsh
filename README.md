# 🫛 psh — the pistachio shell

> The pistachio has one of the hardest shells in the world. It deserved
> a place in the hall of fame next to bash, fish, and zsh. So here it is.

`psh` is a Unix shell written from scratch in C — a real one, with
`fork()` and `exec()` and everything. It follows the **familiar core**
philosophy: your fingers already know it (`ls | grep foo`, `$HOME`,
`&&`), but the classic sh traps are fixed — most importantly, variables
will never word-split, so `rm $file` deletes exactly one file, always.

## Status

Horizon 2 of [the roadmap](docs/ROADMAP.md) — hardened for scripts:
`test`/`[` as fork-free builtins, `type` and `command -v`, `set -e`,
`trap ... EXIT`, and parse errors with line numbers — on a complete
language: `if`/`elif`/`else`, `while`, `for`, `case`, functions with
`$1`..`$9` and a true `$@` splat, `local`/`export`/`unset` on a real
three-tier variable table, `$((...))` arithmetic, `$(...)` command
substitution, `return`/`break`/`continue`, scripts with shebangs,
`psh -c`, `source`, multi-line input — on top of full job control
(`&`, Ctrl-Z, `jobs`/`fg`/`bg`/`wait`), readline editing and history,
tab completion, `~/.pshrc`, pipelines, redirects, `&&`/`||`,
globbing and `#` comments.

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
**basics** (`ll`, `la`, `mkcd`), and **shell-shock**, which — as the
founding documents foretold — goes *XIRT!* when a command fails.
Drive it with `omp list`, `omp enable <plugin>`, `omp theme <t>`.
Plugins register on precmd/chpwd hooks; writing your own is ~20
lines of psh.

## Build & run

```sh
make        # builds ./psh
make test   # runs the smoke tests
./psh       # step into the bag
```

Requires a C compiler, a POSIX system, and GNU readline
(`apt install libreadline-dev` / `dnf install readline-devel`).

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
src/complete.c   tab completion (commands + files)
src/builtins.c   cd, exit, pwd, help, …
src/pistachio.c  🫛 easter pistachios live here, and only here
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

MIT (see LICENSE). Binaries linked with GNU readline are distributed
under GPL terms as readline requires.

---

*NTT layihəsidir. Sistem indi duzludur.* 🫛
