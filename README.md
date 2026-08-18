# 🫛 psh — the pistachio shell

> The pistachio has one of the hardest shells in the world. It deserved
> a place in the hall of fame next to bash, fish, and zsh. So here it is.

`psh` is a Unix shell written from scratch in C — a real one, with
`fork()` and `exec()` and everything. It follows the **familiar core**
philosophy: your fingers already know it (`ls | grep foo`, `$HOME`,
`&&`), but the classic sh traps are fixed — most importantly, variables
will never word-split, so `rm $file` deletes exactly one file, always.

## Status

Milestone 5 of [the roadmap](docs/ROADMAP.md) — psh is a language:
`if`/`elif`/`else`, `while`, `for`, functions with `$1`..`$9`,
`return`/`break`/`continue`, command substitution `$(...)`, scripts
with shebangs, `psh -c`, `source`, multi-line input with a `  > `
continuation prompt — on top of full job control (`&`, Ctrl-Z,
`jobs`/`fg`/`bg`/`wait`), readline editing and history, tab
completion, `~/.pshrc`, pipelines, redirects, `&&`/`||`, variables,
globbing and `#` comments.

Two founding rules, both real: a `$VAR` NEVER word-splits
(`F="two words"; rm $F` deletes exactly one file), and `$(...)`
splits on newlines only (`for f in $(ls)` iterates lines, spaces in
filenames survive).

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
src/expand.c     $VAR, $1..$9, $(...), ~, quote removal, globbing
src/exec.c       tree walker: pipelines, control flow, functions
src/jobs.c       job control: process groups, tcsetpgrp, Ctrl-Z
src/complete.c   tab completion (commands + files)
src/builtins.c   cd, exit, pwd, help, …
src/pistachio.c  🫛 easter pistachios live here, and only here
docs/ROADMAP.md  where this is going
extras/lore/     the sacred founding documents (oh-my-pistachio era)
```

## Salt levels

Some behavior in this shell is nutritionally unnecessary. It is all
contained in `src/pistachio.c` and guaranteed never to affect
correctness. Try `help` and read closely.

---

*NTT layihəsidir. Sistem indi duzludur.* 🫛
