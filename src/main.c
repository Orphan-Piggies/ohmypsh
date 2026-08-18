/*
 * main.c — the REPL (read, evaluate, print, loop).
 *
 * This file IS the definition of a shell: an infinite loop that reads a
 * line of text, parses it, runs it, and remembers the exit status. Every
 * feature psh will ever grow (pipes, job control, scripting) hangs off
 * this loop.
 */
#define _POSIX_C_SOURCE 200809L

#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psh.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/*
 * The prompt. Shows the current directory (with $HOME shortened to ~,
 * like every shell since csh) and, when the previous command failed,
 * its exit status in red — a fish idea worth stealing: failure should
 * be visible without typing `echo $?`.
 */
static void print_prompt(int last_status)
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

    printf(PSH_NUT " \033[32m%s\033[0m", disp);
    if (last_status != 0)
        printf(" \033[31m[%d]\033[0m", last_status);
    printf(" $ ");
    fflush(stdout);
}

int main(void)
{
    /*
     * Only behave interactively (banner, prompt) when stdin is a
     * terminal. When input is piped in — `echo ls | psh` or a test
     * script — we stay silent, which is also how bash decides.
     */
    bool interactive = isatty(STDIN_FILENO);

    /*
     * The shell itself must survive Ctrl-C; only the command it is
     * currently running should die. So the shell ignores SIGINT here,
     * and exec.c restores the default action in each child after fork.
     * Forgetting this is the classic beginner-shell bug: one Ctrl-C
     * kills your whole shell.
     */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    if (interactive)
        psh_pistachio_hello();

    char *line = NULL;
    size_t linecap = 0;
    int status = 0;

    for (;;) {
        if (interactive)
            print_prompt(status);

        /* getline() grows the buffer for us and returns -1 on EOF. */
        if (getline(&line, &linecap, stdin) < 0) {
            if (interactive)
                puts("exit"); /* echo what Ctrl-D means, like bash */
            break;
        }

        psh_command *cmd = psh_parse_line(line);
        if (!cmd) {
            fprintf(stderr, "psh: syntax error: unterminated quote\n");
            status = 2;
            continue;
        }

        status = psh_execute(cmd);
        psh_command_free(cmd);
    }

    free(line);
    return status;
}
