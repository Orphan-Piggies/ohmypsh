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

## M4 — daily-driver hardening (mostly ✅)
- [x] Job control: `&`, Ctrl-Z, `jobs`, `fg [%n]`, `bg`, `wait`
      (process groups, tcsetpgrp, WUNTRACED, per-job termios — see the
      jobs.c header comment for the four-sentence mental model)
- [x] Background &&/|| lists run in a forked subshell (`a && b &`)
- [x] `~/.pshrc` startup file (sourced interactively, line by line)
- [x] Tab completion: commands (builtins + $PATH) in command position,
      readline's filename completion everywhere else
- [x] `#` comments (bonus: needed for a civilized .pshrc)
- [ ] Set as login shell — user ritual, not code:
      `echo $(pwd)/psh | sudo tee -a /etc/shells && chsh -s $(pwd)/psh`
      (recommended only after some days of daily driving)

## M5 — a language
- [ ] `if` / `else`, `for`, `while`, functions
- [ ] Command substitution: `$(...)`
- [ ] Scripts: `psh file.psh`, shebang support

## M6 — oh-my-psh, for real
- [ ] The name comes full circle: a plugin/theme framework ON psh,
      written IN psh. Antep Mode ships as the default theme.
