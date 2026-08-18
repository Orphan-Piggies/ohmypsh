/*
 * exec.c — actually run a command.
 *
 * This is the heart of every Unix shell, and it is exactly three system
 * calls:
 *
 *   fork()    clone this process; both copies continue from here
 *   execvp()  (in the child) replace yourself with the target program;
 *             the 'p' means "search $PATH for it"
 *   waitpid() (in the parent) sleep until the child finishes, collect
 *             its exit status
 *
 * Builtins are the exception: `cd` MUST run inside the shell process,
 * because a child changing ITS directory and then exiting would change
 * nothing for us — each process has its own working directory.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "psh.h"

int psh_execute(psh_command *cmd)
{
    if (!cmd || cmd->argc == 0)
        return 0; /* empty line: not an error */

    psh_builtin_fn builtin = psh_find_builtin(cmd->argv[0]);
    if (builtin)
        return builtin(cmd->argv);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "psh: fork: %s\n", strerror(errno));
        return 1;
    }

    if (pid == 0) {
        /*
         * Child. The shell ignores SIGINT/SIGQUIT so Ctrl-C can't kill
         * it — but the command we run SHOULD die on Ctrl-C, so restore
         * the defaults before exec. exec keeps signal dispositions.
         */
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);

        execvp(cmd->argv[0], cmd->argv);

        /* execvp only ever returns on failure. */
        if (errno == ENOENT)
            fprintf(stderr, "psh: %s: command not found — %s\n",
                    cmd->argv[0], psh_pistachio_notfound());
        else
            fprintf(stderr, "psh: %s: %s\n",
                    cmd->argv[0], strerror(errno));
        /*
         * _exit, not exit: we are a forked copy of the shell and must
         * not flush the shell's stdio buffers a second time. 127/126
         * are the conventional codes for not-found / not-executable.
         */
        _exit(errno == ENOENT ? 127 : 126);
    }

    /* Parent: wait for the child and translate its status. */
    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0) {
        fprintf(stderr, "psh: waitpid: %s\n", strerror(errno));
        return 1;
    }
    if (WIFSIGNALED(wstatus)) {
        int sig = WTERMSIG(wstatus);
        if (sig == SIGINT)
            fputc('\n', stdout); /* land the next prompt on a fresh line */
        return 128 + sig; /* bash convention: killed by signal N → 128+N */
    }
    return WEXITSTATUS(wstatus);
}
