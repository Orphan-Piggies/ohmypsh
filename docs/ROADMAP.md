# psh roadmap 🫛

Each milestone leaves the shell in a working, buildable state. The rule:
never start a milestone until the previous one survives daily poking.

## M1 — a shell exists ✅
- [x] REPL: prompt → read line → run → repeat
- [x] Lexer with single/double quotes
- [x] fork + execvp + waitpid, correct exit statuses (127/126/128+sig)
- [x] Builtins: `cd` (incl. `cd -`), `exit`, `pwd`, `help`
- [x] Shell survives Ctrl-C; children don't
- [x] Prompt shows cwd (~-shortened) and last failure status
- [x] `pistachio.c` established as the easter-pistachio containment zone

## M2 — plumbing (the week-1 goal) ✅
- [x] Pipes: `ls | grep foo | wc -l` (loop of fork/exec with pipe(2),
      parent closes both ends, wait for all children)
- [x] Redirects: `> file`, `>> file`, `< file`, `2> file`
- [x] Multiple commands per line: `;`
- [x] Lexer grows operator tokens (`|`, `>`, `<`, `;`) — and parser.c
      is born: tokens → statements → pipelines → commands
- [x] Lone builtins with redirects run in-parent (fd save/restore);
      builtins inside pipelines run in the child, like bash

## M3 — livable (the week-2 goal) ✅
- [x] GNU readline: ↑/↓ history (persisted to ~/.psh_history), Ctrl-R,
      Ctrl-L, line editing — interactive only; scripts keep getline
- [x] `&&` and `||` chaining with sh precedence: `a | b && c` is
      `(a | b) && c`; skipped pipelines preserve $?
- [x] Globbing via glob(3): `*.c` — no-match stays literal, any
      quoting suppresses it
- [x] Variables: `NAME=value`, `$NAME`, `${NAME}`, `$?`, `$$`,
      `A=1 cmd` per-command env — values NEVER word-split
      (the founding design decision, enforced in expand.c)
- [x] `~` expansion
- [x] expand.c is born: parse first, expand at execution time,
      quote-remove last — the ordering that makes `X=5; echo $X` work

## M4 — daily-driver hardening ✅
- [x] Job control: `&`, Ctrl-Z, `jobs`, `fg [%n]`, `bg`, `wait`
      (process groups, tcsetpgrp, WUNTRACED, per-job termios — see the
      jobs.c header comment for the four-sentence mental model)
- [x] Background &&/|| lists run in a forked subshell (`a && b &`)
- [x] `~/.pshrc` startup file (sourced interactively, line by line)
- [x] Tab completion: commands (builtins + $PATH) in command position,
      readline's filename completion everywhere else
- [x] `#` comments (bonus: needed for a civilized .pshrc)
- [x] Set as login shell — user ritual, not code: performed 2026-08-18.
      Every terminal now opens into the bag. 🫛

## M5 — a language ✅
- [x] `if` / `elif` / `else`, `while`, `for` — recursive-descent
      parser; keywords only special in command position, like sh
- [x] Functions: `name() { ...; }` with `$1`..`$9`, `$#`, `$0`;
      functions shadow builtins shadow $PATH
- [x] `return` / `break` / `continue` (flow flags, no longjmp)
- [x] Command substitution `$(...)` — splits on NEWLINES only
      (fish's rule): `for f in $(ls)` iterates lines, $VAR still
      never splits
- [x] Multi-line input: parse answers "incomplete" → `  > `
      continuation prompt interactively, clean error at script EOF
- [x] Scripts: `psh file.psh args`, shebang (via `#` comments),
      `psh -c 'cmd'`, `source`/`.` builtin
- [x] Interactive Ctrl-C interrupts pure-builtin loops
      (`while true; do true; done` is killable)

## M6 — oh-my-psh, for real ✅
- [x] The name comes full circle: a plugin/theme framework ON psh,
      written IN psh (`omp/`). Antep Mode ships as the default theme.
- [x] C-side enablers: PSH_PROMPT (template re-expanded per prompt —
      $( ) in your prompt is just command substitution), the
      omp_precmd hook, top-level `return` in sourced files
- [x] Themes: antep (green + git branch + status segment), plain
- [x] Plugins: basics (ll/la/mkcd), git (branch segment + gs/gl/gd),
      shell-shock ("XIRT!" after failures — the founding prophecy)
- [x] `./psh omp/install.psh` — the installer, itself a psh script

# ⛵ Endless Horizons

The original roadmap is complete; these are the seas beyond. Decided
2026-08-18: sail H1 first, on a REAL variable table; keep readline
for now; license chosen when H5 approaches (note: linking GNU
readline requires a GPL-compatible choice).

## H1 — finish the language ✅ (v0.7.0)
- [x] Real variable table (vars.c): locals → shell vars → environ;
      `export` promotes; children inherit only exported ones;
      inherited env names (PATH...) stay exported on assignment
- [x] `local` — dynamic scope, restored on return; a local shadowing
      an env-visible name mirrors into the environ for the call
- [x] `export`, `unset` builtins
- [x] `$@` splat — a word that IS $@ expands to a LIST, one word per
      argument, never re-split; embedded $@ joins with spaces
- [x] `case ... esac` — fnmatch patterns, `a|b)` alternation,
      optional `(`, last `;;` optional
- [x] `$((...))` arithmetic (arith.c): + - * / % comparisons && ||,
      bare variable names, long long — `$(expr ...)` retired

## H2 — scripting hardening ✅ (v0.8.0)
- [x] `test` / `[` as builtins (testcmd.c): string/numeric/file ops,
      `!`, `-a`/`-o` — a 1000-iteration `[ ]` loop runs in ~25ms,
      down from a fork per comparison
- [x] `type` and `command -v` builtins — functions/builtins/$PATH;
      `command -v` is silent+status-1 when missing, so plugins can
      probe for tools
- [x] `set -e` (untested single-pipeline failures exit; if/while
      conditions and &&/|| operands don't count) and `+e`
- [x] `trap 'cmds' EXIT` / `trap - EXIT` — fires on every exit path
      of THIS shell only (children _exit past it; pid-guarded)
- [x] Parse errors with line numbers in scripts and multi-line input

## H3 — the omp plugin fleet ✅ (v0.9.0, pure psh)
- [x] Hook engine in omp.psh: omp_hook_precmd / omp_hook_chpwd —
      plugins register instead of overwriting each other;
      $__OMP_STATUS carries the last status to every hook
- [x] `omp` CLI: list / enable / theme / reload
- [x] python: venv AUTO-ACTIVATION — upward .venv/venv search on
      every directory change, PATH save/restore, ⌁name prompt
      segment, venv-init
- [x] django (dj/djrun/djmig/djtest), flask (fl/flrun/flsh)
- [x] rails (r prefers bin/rails; rs/rc/rg/rmig/rrout)
- [x] docker (dps/dim/dlog/dex/dstop/dclean)
- [x] npm (ni/nr/ns/nt) + opt-in ⬢ node-version segment
- [x] git v2: ✗ dirty marker, ↑ahead ↓behind arrows, ga/gc/gp
- [x] C bonus: hyphenated function names (venv-init) now parse,
      as in bash/zsh

## H4 — the cockpit (parked, revisit after H1–H3)
- [ ] Hand-rolled raw-mode line editor → fish-style autosuggestions
      and syntax highlighting; drops the readline dependency

## H5 — seaworthiness (v0.10.0, mostly ✅)
- [x] LICENSE: MIT (source), with the readline/GPL note for binaries
- [x] CI: GitHub Actions — build with -Werror, smoke tests, ASan
      suite, quick fuzz pass on every push
- [x] Sanitizer pass: `make test-asan` — whole suite under
      AddressSanitizer + LeakSanitizer, zero errors, zero leaks
- [x] Fuzzer (tests/fuzz.sh): urandom + token soup + mutated
      programs; 2000 inputs, zero crashes; knows the difference
      between a crash and an honest infinite loop
- [x] man page (docs/psh.1), installed by `make install`
- [x] Antep Mode screenshots for the README (a human with a terminal
      that has good taste delivered three: dirty marker, venv
      auto-activation, XIRT — docs/screenshots/)
- [ ] Packaging: deb / AUR (when there's a public repo to point at)
