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

## M3 — livable (the week-2 goal)
- [ ] GNU readline: history, arrow keys, Ctrl-R, line editing for free
      (link with `-lreadline`; a hand-rolled editor can replace it later)
- [ ] `&&` and `||` chaining
- [ ] Globbing via glob(3): `*.c`, `src/*.o`
- [ ] Variables: `NAME=value`, `$NAME`, `$?` — values never word-split
      (the founding design decision, see README)
- [ ] `~` expansion

## M4 — daily-driver hardening
- [ ] Job control: `&`, Ctrl-Z, `jobs`, `fg`, `bg` (process groups,
      tcsetpgrp, SIGTSTP — the hardest classic-Unix material in the project)
- [ ] `~/.pshrc` startup file
- [ ] Tab completion (filenames first, commands later)
- [ ] Set as login shell: add to /etc/shells, chsh

## M5 — a language
- [ ] `if` / `else`, `for`, `while`, functions
- [ ] Command substitution: `$(...)`
- [ ] Scripts: `psh file.psh`, shebang support

## M6 — oh-my-psh, for real
- [ ] The name comes full circle: a plugin/theme framework ON psh,
      written IN psh. Antep Mode ships as the default theme.
