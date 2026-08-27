# 🫛 psh — the pistachio shell

[![CI](https://github.com/marmeladze/ohmypsh/actions/workflows/ci.yml/badge.svg)](https://github.com/marmeladze/ohmypsh/actions/workflows/ci.yml)

> The pistachio has one of the hardest shells in the world. It deserved
> a place in the hall of fame next to bash, fish, and zsh. So here it is.

`psh` is a Unix shell written from scratch in C — a real one, with
`fork()` and `exec()` and everything. It follows the **familiar core**
philosophy: your fingers already know it (`ls | grep foo`, `$HOME`,
`&&`), but the classic sh traps are fixed. Zero dependencies: the
binary needs libc, full stop.

## The two founding rules

```sh
F="two words"; rm $F       # deletes exactly ONE file — $VAR never word-splits
for f in $(ls); do …       # $( ) splits on newlines only; spaces survive
```

Everything else follows from taking those seriously.

## A taste

```sh
exec 3<> /dev/tcp/127.0.0.1/6379    # sockets are just fds
echo PING >&3
read -r reply <&3                    # one byte at a time: exactly one line
[ "$reply" = +PONG ] && echo up      # no ghost \r — the shell strips it

echo ${file%.tar.gz}                 # parameter ops, heredocs, pipefail,
set -eu -o pipefail                  # $PIPESTATUS, readonly — all aboard

?? find videos over 100M from last week
# → the AI's answer lands IN your prompt: editable, highlighted,
#   run only when YOU press Enter. Works offline via ollama.
```

## What's inside

- **The cockpit** — a hand-rolled raw-mode line editor: fish-style
  autosuggestions from your history, syntax highlighting straight
  from the shell's *real* lexer (typos glow red before Enter),
  Ctrl-R search, tab completion, bracketed paste, multi-line
  editing, Ctrl-X Ctrl-E.
- **A complete language** — `if`/`while`/`for`/`case`, functions, a
  true `$@` splat, three-tier variables with `local`/`export`/
  `readonly`, `$((…))` arithmetic, heredocs, `${var#pat}`-family
  operators, `set -eu -o pipefail`, `$PIPESTATUS`, `trap EXIT`,
  full job control.
- **The wire** — numbered-fd redirections (`2>&1`, `3<>`, `N>&M`),
  `exec` and `read` builtins, and `/dev/tcp/HOST/PORT` sockets —
  proven against a live redis by a pure-psh client plugin.
- **The armory** — three standalone tools, below.

Prefer watching to reading? The
[**psh Flow Theater**](https://claude.ai/code/artifact/abbdfe69-bbd9-4b4f-bb12-4f4770f01f40)
animates the internals: a keystroke becoming a fork, a PING riding
`/dev/tcp` to redis and back, Ctrl-Z's journey through the kernel.

## 🫛 oh-my-psh

The framework this project was named for — written in psh, running
on psh. Themes are prompt templates; plugins register on
precmd/chpwd hooks; writing your own is ~20 lines.

```sh
./psh omp/install.psh    # sets up ~/.pshrc; then: omp list
```

| plugin | what it does |
|---|---|
| **python** | venvs auto-activate when you `cd` in — never type `source .venv/bin/activate` again |
| **agent** | `?? <plain words>` → an AI CLI's answer staged in your prompt; `oops` explains the last failure |
| **redis** | `rget`/`rset`/`rmon` — a real client over `exec 9<>/dev/tcp`, no redis-cli |
| **git** | branch segment with ✗ dirty marker and ↑↓ arrows, plus `ga`/`gc`/`gp` |
| **z** | frecency directory jumping |
| **shell-shock** | goes *XIRT!* when a command fails, as the founding documents foretold |

…plus django, flask, rails, docker, npm, extract, duration, basics.

Antep Mode in the wild — the git segment catching a tree going dirty:

![touch a file and the ✗ dirty marker appears](docs/screenshots/dirty-stage.png)

a venv activating itself on `cd`:

![cd into a project and its venv activates, ⌁ segment appears](docs/screenshots/venv-discovery.png)

and shell-shock keeping morale up:

![a typo gets a flavor line, XIRT!, and a red 127 status segment](docs/screenshots/xirt.png)

## The armory

**🧂 salt** — cat with colors, under one rule: **the bytes are never
changed, only colored.** Copying salted output yields exact source;
piped output is byte-identical to the input; `tail -f log | salt -l
yaml` highlights live. Thirteen languages, `-L` lists them.

**🔥 roast** — salt's opposite number: Markdown *rendered*. Headings
lose their `#`s, `- [ ]` becomes ☐/☑, tables align into ruled
columns, links become real hyperlinks, fenced code keeps salt's
highlighting while the fences vanish. `roast docs/ROADMAP.md` —
checkboxes look like checkboxes.

**🔒 cage** — run the sketchy thing FIRST: `cage ./installer.sh`
executes anything against a kernel-enforced **read-only
filesystem** (Landlock, unprivileged), writes surviving only in a
kept scratch dir you inspect afterwards. On kernels without
Landlock it refuses rather than silently running uncaged.

```sh
salt src/exec.c                     # colors, and only colors
tail -f NOTES.md | roast            # a document, live
cage -w build make                  # build allowed, nothing else
```

## Build & install

```sh
make          # psh + salt + roast + cage; needs a C compiler, nothing else
make test     # the suites
sudo make install               # /usr/local: binaries, man pages, omp
make deb                        # Debian/Ubuntu package (dpkg-deb only)
```

A prebuilt `.deb` ships with each
[release](https://github.com/marmeladze/ohmypsh/releases); Arch
users find the PKGBUILD in `packaging/aur/` (package name
**ohmypsh** — the AUR name `psh` was taken — installing
`/usr/bin/psh`).

## Layout

```
src/main.c       the REPL, scripts, -c, ~/.pshrc, multi-line input
src/lexer.c      input → tokens (words raw; quotes and $() intact)
src/parser.c     recursive descent: if/while/for/functions/lists
src/expand.c     $VAR, ${ops}, $@, $(...), $((...)), ~, quotes, glob
src/vars.c       locals → shell vars → environ; export/local/readonly
src/arith.c      the $(( ... )) evaluator
src/testcmd.c    the test / [ builtin (fork-free conditions)
src/exec.c       tree walker: pipelines, control flow, functions
src/jobs.c       job control: process groups, tcsetpgrp, Ctrl-Z
src/editor.c     the cockpit: autosuggestions, highlighting, ^R, ^X^E
src/complete.c   completion candidates (commands + files)
src/builtins.c   cd, exec, read, readonly, help, …
src/pistachio.c  🫛 easter pistachios live here, and only here
tools/           the armory: salt.c, roast.c, cage.c, hl.h
omp/             oh-my-psh: themes/, plugins/, install.psh — in psh
docs/ROADMAP.md  the whole story, milestone by milestone
extras/lore/     the sacred founding documents (oh-my-pistachio era)
```

## Salt levels

Some behavior in this shell is nutritionally unnecessary. It is all
contained in `src/pistachio.c` and guaranteed never to affect
correctness. Try `help` and read closely.

## Quality

`make test` — 246 checks across four suites (shell, salt, roast,
cage) — plus `make test-asan` (the same under AddressSanitizer +
LeakSanitizer: zero errors, zero leaks), a pty-driven editor suite
(`make test-editor`, 26 keystroke tests), and a fuzzer that has
thrown thousands of hostile inputs without a crack. CI runs the lot
on every push.

## License

MIT (see LICENSE) — source and binaries alike, now that the last
GPL-licensed link went overboard.

---

