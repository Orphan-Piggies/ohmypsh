/*
 * main.c — the REPL (read, evaluate, print, loop).
 *
 * Milestone 3 swaps getline() for GNU readline when stdin is a
 * terminal, and suddenly the shell feels alive: ↑/↓ history, Ctrl-R
 * search, Ctrl-L clear, Emacs-style line editing — all inherited from
 * the same library bash uses. Non-interactive input (pipes, scripts)
 * keeps the plain getline() path; readline would only get in the way.
 */
#define _POSIX_C_SOURCE 200809L

#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <readline/history.h>
#include <readline/readline.h>

#include "psh.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* What $? expands to; also the shell's own exit status. */
int psh_last_status = 0;

static char hist_path[PATH_MAX];

static void save_history(void)
{
    if (hist_path[0])
        write_history(hist_path);
}

/*
 * Build the prompt string. Shows the current directory (with $HOME
 * shortened to ~) and, when the previous command failed, its status
 * in red — failure should be visible without typing `echo $?`.
 *
 * The \001/\002 bytes bracket the color escapes: they tell readline
 * "these bytes are invisible", so it can compute the prompt's true
 * width. Without them, line editing garbles after the first ↑.
 */
static void build_prompt(char *out, size_t outsz)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "?");

    const char *home = getenv("HOME");
    size_t hlen = home ? strlen(home) : 0;
    const char *disp = cwd;
    char shortened[PATH_MAX + 2];
    if (home && strncmp(cwd, home, hlen) == 0 &&
        (cwd[hlen] == '/' || cwd[hlen] == '\0')) {
        snprintf(shortened, sizeof shortened, "~%s", cwd + hlen);
        disp = shortened;
    }

    if (psh_last_status == 0)
        snprintf(out, outsz,
                 PSH_NUT " \001\033[32m\002%s\001\033[0m\002 $ ", disp);
    else
        snprintf(out, outsz,
                 PSH_NUT " \001\033[32m\002%s\001\033[0m\002 "
                 "\001\033[31m\002[%d]\001\033[0m\002 $ ",
                 disp, psh_last_status);
}

static void run_line(const char *line)
{
    /* lex → parse → execute; each stage reports its own errors */
    bool err = false;
    psh_token *tokens = psh_tokenize(line, &err);
    if (err) {
        psh_last_status = 2;
        return;
    }

    psh_stmt *stmts = psh_parse(tokens, &err);
    psh_tokens_free(tokens);
    if (err) {
        psh_last_status = 2;
        return;
    }

    if (stmts) { /* NULL = blank line: nothing to do */
        psh_last_status = psh_execute(stmts);
        psh_stmts_free(stmts);
    }
}

int main(void)
{
    bool interactive = isatty(STDIN_FILENO);

    /*
     * The shell itself must survive Ctrl-C; only the command it is
     * currently running should die. So the shell ignores SIGINT here,
     * and exec.c restores the default action in each child after fork.
     */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    if (interactive) {
        psh_pistachio_hello();

        const char *home = getenv("HOME");
        if (home) {
            snprintf(hist_path, sizeof hist_path, "%s/.psh_history", home);
            read_history(hist_path);
            stifle_history(1000);
            /* atexit catches BOTH exit paths: Ctrl-D and the `exit`
             * builtin (which calls exit() from deep in builtins.c). */
            atexit(save_history);
        }

        char prompt[PATH_MAX + 64];
        for (;;) {
            build_prompt(prompt, sizeof prompt);
            char *line = readline(prompt);
            if (!line) { /* Ctrl-D on an empty line */
                puts("exit");
                break;
            }
            if (*line)
                add_history(line);
            run_line(line);
            free(line);
        }
    } else {
        char *line = NULL;
        size_t linecap = 0;
        while (getline(&line, &linecap, stdin) >= 0)
            run_line(line);
        free(line);
    }

    return psh_last_status;
}
