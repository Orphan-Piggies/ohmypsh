# 🫛 psh — the pistachio shell

> The pistachio has one of the hardest shells in the world. It deserved
> a place in the hall of fame next to bash, fish, and zsh. So here it is.

`psh` is a Unix shell written from scratch in C — a real one, with
`fork()` and `exec()` and everything. It follows the **familiar core**
philosophy: your fingers already know it (`ls | grep foo`, `$HOME`,
`&&`), but the classic sh traps are fixed — most importantly, variables
will never word-split, so `rm $file` deletes exactly one file, always.

## Status

Milestone 2 of [the roadmap](docs/ROADMAP.md): a working interactive
shell with quoting, pipelines (`ls | grep c | wc -l`), redirects
(`>`, `>>`, `<`, `2>`), `;`-separated commands, and `cd`/`exit`/`pwd` —
and it survives Ctrl-C like a shell should. Next up: readline (history,
arrow keys), `&&`/`||`, globbing, and variables.

## Build & run

```sh
make        # builds ./psh
make test   # runs the smoke tests
./psh       # step into the bag
```

Requires only a C compiler and a POSIX system. No dependencies yet
(GNU readline arrives in milestone 3).

## Layout

```
src/main.c       the REPL — the loop that IS the shell
src/lexer.c      line → tokens (words, | ; < > >> 2>), quote handling
src/parser.c     tokens → statements → pipelines → commands
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
