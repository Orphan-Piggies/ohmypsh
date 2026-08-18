# 🫛 psh — the pistachio shell

> The pistachio has one of the hardest shells in the world. It deserved
> a place in the hall of fame next to bash, fish, and zsh. So here it is.

`psh` is a Unix shell written from scratch in C — a real one, with
`fork()` and `exec()` and everything. It follows the **familiar core**
philosophy: your fingers already know it (`ls | grep foo`, `$HOME`,
`&&`), but the classic sh traps are fixed — most importantly, variables
will never word-split, so `rm $file` deletes exactly one file, always.

## Status

Milestone 3 of [the roadmap](docs/ROADMAP.md) — psh is now livable:
readline line editing (↑/↓ history saved to `~/.psh_history`, Ctrl-R,
Ctrl-L), pipelines, redirects (`>`, `>>`, `<`, `2>`), `&&`/`||`/`;`,
variables (`NAME=value`, `$NAME`, `$?`, `A=1 cmd`), globbing, and `~`.
And the founding rule is real: `F="two words"; rm $F` deletes exactly
one file. Next up: job control, `~/.pshrc`, and tab completion.

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
src/main.c       the REPL — the loop that IS the shell
src/lexer.c      line → tokens (words kept raw, | || && ; < > >> 2>)
src/parser.c     tokens → statements → &&/|| lists → pipelines → commands
src/expand.c     $VAR, ~, quote removal, globbing — at execution time
src/exec.c       fork / execvp / waitpid, pipes, redirects
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
