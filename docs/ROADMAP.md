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

## H4 — the cockpit (unparked 2026-08-18; staged like the M-days)
Hand-rolled raw-mode line editor → fish-style autosuggestions and
syntax highlighting; drops the readline dependency. Readline stays
the DEFAULT until parity — the cockpit is opt-in via PSH_EDITOR=nut
(checked every prompt, so it toggles live).
- [x] H4.1 editor.c: termios raw mode (TCSADRAIN — type-ahead
      survives), multi-row repaint renderer wrapping at the terminal
      width, UTF-8 codepoint cursor with wcwidth display math (ə is
      one column, 🫛 is two), emacs keys (^A^E^B^F^K^U^W^T, arrows,
      Home/End/Delete, Alt-b/f, Ctrl-arrows), ↑↓ history sharing
      ~/.psh_history with readline, SIGWINCH, Ctrl-C aborts the
      line. tests/editor.sh drives it through a real pty
      (make test-editor; local for now — pty timing on CI is jittery)
- [x] H4.2 complete.c split into a readline-free candidate ENGINE
      (commands + new filename completer, sorted, dirs get '/') and
      thin readline glue; cockpit Tab: unique → insert (+space),
      else extend common prefix, else list in columns (capped 120);
      Ctrl-R incremental reverse search (the search UI is just a
      temporary prompt — same renderer); bracketed paste: multi-line
      pastes land IN the buffer, Enter submits the lot (renderer
      learned embedded newlines via the cell-walk rewrite — which
      made multi-line EDITING work too)
- [x] H4.2b ^X^E: edit the line in $VISUAL/$EDITOR (zsh-style — the
      result returns to the buffer, Enter submits; bash's
      execute-on-exit trusts vim muscle memory a bit too much)
- [x] H4.3 the prizes. Autosuggestions: newest history entry the
      buffer prefixes, painted grey past the cursor; → / End / ^E
      accept it all, Alt-f / Ctrl-→ one word. Syntax highlighting by
      the REAL lexer (tokens grew a byte offset — no second grammar
      to drift): command position green if it resolves
      (builtin/function/$PATH via psh_path_lookup) and red if not —
      typos glow before Enter; keywords bold; strings yellow; $vars,
      assignments and function-def names cyan; comments grey
- [x] H4.4 (v0.11.0) the flip: the cockpit IS the editor — readline
      overboard (LDLIBS empty; the binary depends on libc, full
      stop), LICENSE un-noted (pure MIT, source AND binaries), CI no
      longer installs libreadline-dev, README/man updated,
      PSH_EDITOR retired. complete.c is pure engine now.

## H6 — the crunchy fleet (opened 2026-08-18, after H4)
Themes and plugins worth stealing from the neighbours (omz/omf),
now that we own the whole stack down to the line editor.
- [x] z — frecency directory jumping: chpwd hook logs to ~/.psh_z,
      `z <query>` cds to the most-visited match (ties → most recent)
- [x] extract / x — one command cracks any archive; the plugin is
      one big case/esac, which is exactly the point
- [x] C enabler: the REPL times every command → $PSH_CMD_MS;
      duration plugin turns it into a ⏱2.4s prompt segment
      (threshold $OMP_DURATION_MIN_MS, default 2s)
- [x] themes from the agent suggestion pile (extras/lore/), salt
      applied — picked madera (agro-industrial matrix mono-green,
      shows every ms) and aleppo (fustuq al-halabi silk-road
      vintage); rejected the palette-swaps and the physically
      impossible one
- [ ] candidates: fish-style abbr (editor hook), RPROMPT (we own
      the renderer), bgnotify on $PSH_CMD_MS, the agent plugin
      (?? / oops), command-not-found hook

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

## H7 — the wire (opened 2026-08-26)
The bash /dev/tcp redis trick, earned honestly. Not a redis builtin —
RESP belongs in redis-cli, not in C that rots. The prize is the
generic mechanism: sockets as file descriptors, driven by redirection
syntax we need anyway. Staged so every rung is useful on its own:
- [x] H7.1 redirections grew up: the per-command in/out/err fields
      became an ORDERED list of {fd, kind, target} (order IS the
      semantics: `>f 2>&1` ≠ `2>&1 >f`); lexer learned the POSIX
      IO_NUMBER rule (`echo 2>f` redirects, `echo 2 >f` echoes) and
      one TOK_REDIR token for `N<` `N>` `N>>` `N<>` `N<&` `N>&`;
      dup targets expand (`2>&$FD`), `-` closes; in-parent builtin
      save/restore generalized to any fd (parked CLOEXEC above every
      touched fd). The table stake unlocked: `2>&1`.
- [x] H7.2 `exec` builtin — intercepted in the executor (POSIX
      special: found before functions, never forked): redirections
      apply to the SHELL and stay (`exec 3<>file` gives every later
      command fd 3), `exec cmd` execvp's in place after handing the
      program default signal dispositions. A failed exec is
      survived, like interactive bash. In a pipeline the child just
      becomes the command.
- [x] H7.3 the dessert: `/dev/tcp/host/port` (and `/dev/udp/...`)
      intercepted in the redirect path — getaddrinfo (hostnames,
      service names, numeric IPv6) + socket + connect, tried across
      all addresses; EINTR aborts so a dead host can't brick the
      prompt. SIGPIPE hygiene came with it: the interactive shell
      ignores it (a dead socket reports EPIPE instead of closing
      the tab), children get SIG_DFL back, and a failed `exec cmd`
      now restores the dispositions it actually saved. Smoke suite
      does a LIVE roundtrip: exec 3<>/dev/tcp → echo >&3 → head <&3
      against a local echo server.
- [x] H7.4 `read` builtin is born (`read [-r] NAME...`) — one-byte
      reads ALWAYS, so `read -r line <&3` takes exactly one reply
      from a socket and leaves the rest (`head` gulps 8K buffers
      and eats replies — works in demos, loses data daily). psh
      flavor, on purpose: ONE name takes the line verbatim (the
      founding never-split rule; several names split on blanks,
      last takes the rest), and a trailing \r is stripped — every
      line protocol ends \r\n and the ghost \r bites everyone in
      sh. EOF delivers the partial line with status 1; Ctrl-C is
      130. Verified against a LIVE containerized redis: PING →
      +PONG compares CLEAN, and a burst of two commands reads back
      as two exact replies.

H7 closed 2026-08-27, all four rungs. The blog snippet that opened
it runs verbatim — and the payoff shipped the same day:
omp/plugins/redis.psh, a real client in ~120 lines of pure psh
(rping/rget/rset/rdel/rincr/rkeys/rcmd/rmon + a silent-probe ⛁
prompt segment), verified live against a containerized redis 7.
Lesson learned en route: case patterns quote-strip before fnmatch,
so a literal star is spelled [*].
