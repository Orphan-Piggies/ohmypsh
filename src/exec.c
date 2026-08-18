/*
 * exec.c — run a syntax tree.
 *
 * Milestone 1 was fork + execvp + waitpid for one command. Milestone 2
 * adds the two great plumbing tricks of Unix, both built on the same
 * idea — file descriptors are just numbered slots, and dup2() lets you
 * put anything in slot 0 (stdin), 1 (stdout) or 2 (stderr):
 *
 *   redirect:  open() a file, dup2() it onto the right slot
 *   pipe:      pipe() makes a connected read/write fd pair; the writer
 *              dup2()s one end onto its stdout, the reader dup2()s the
 *              other onto its stdin. The kernel does the rest.
 *
 * A pipeline `a | b | c` is: for each stage, make a pipe to the next
 * stage (if any), fork, wire the child's fds, exec. All stages run
 * CONCURRENTLY — that's the point of pipes — and the pipeline's exit
 * status is the LAST stage's status (the bash convention).
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "psh.h"

/*
 * Splice this command's redirect files onto fd 0/1/2. Called in the
 * child right before exec (or in the parent, wrapped with save/restore,
 * when a lone builtin has redirects). Explicit redirects run AFTER
 * pipe wiring, so `a | b > f` sends b's output to f — like sh.
 */
static int apply_redirs(const psh_command *c)
{
    if (c->in_path) {
        int fd = open(c->in_path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "psh: %s: %s\n", c->in_path, strerror(errno));
            return -1;
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->out_path) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open(c->out_path, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "psh: %s: %s\n", c->out_path, strerror(errno));
            return -1;
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    if (c->err_path) {
        int fd = open(c->err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "psh: %s: %s\n", c->err_path, strerror(errno));
            return -1;
        }
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    return 0;
}

/*
 * A lone builtin must run in the shell process (that's what makes it a
 * builtin) — so to honor `pwd > file` we temporarily rewire the
 * SHELL'S own fds and put them back afterwards. dup() saves a spare
 * copy of each slot we're about to clobber.
 */
static int run_builtin_in_parent(psh_builtin_fn fn, const psh_command *c)
{
    int saved[3] = { -1, -1, -1 };
    if (c->in_path)
        saved[0] = dup(STDIN_FILENO);
    if (c->out_path)
        saved[1] = dup(STDOUT_FILENO);
    if (c->err_path)
        saved[2] = dup(STDERR_FILENO);

    int status = (apply_redirs(c) < 0) ? 1 : fn(c->argv);

    /* Flush BEFORE restoring, or buffered output lands on the old fd. */
    fflush(stdout);
    fflush(stderr);
    for (int i = 0; i < 3; i++) {
        if (saved[i] >= 0) {
            dup2(saved[i], i);
            close(saved[i]);
        }
    }
    return status;
}

static int wait_status_to_exit(int wstatus)
{
    if (WIFSIGNALED(wstatus)) {
        int sig = WTERMSIG(wstatus);
        if (sig == SIGINT)
            fputc('\n', stdout); /* land the next prompt on a fresh line */
        return 128 + sig; /* bash convention: killed by signal N → 128+N */
    }
    return WEXITSTATUS(wstatus);
}

static int run_pipeline(const psh_command *first)
{
    /*
     * A single builtin with no pipe runs in-process — this is the only
     * way `cd` and `exit` can affect the shell. A builtin INSIDE a
     * pipeline runs in a forked child instead, which is why
     * `cd /tmp | cat` moves nobody in psh, bash, or zsh.
     */
    if (!first->next && first->argc > 0) {
        psh_builtin_fn fn = psh_find_builtin(first->argv[0]);
        if (fn)
            return run_builtin_in_parent(fn, first);
    }

    size_t nstages = 0;
    for (const psh_command *c = first; c; c = c->next)
        nstages++;
    pid_t *pids = malloc(nstages * sizeof *pids);
    if (!pids) {
        fprintf(stderr, "psh: out of memory\n");
        return 1;
    }

    size_t spawned = 0;
    int prev_read = -1; /* read end of the pipe from the previous stage */

    for (const psh_command *c = first; c; c = c->next) {
        int fds[2] = { -1, -1 };
        if (c->next && pipe(fds) < 0) {
            fprintf(stderr, "psh: pipe: %s\n", strerror(errno));
            break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "psh: fork: %s\n", strerror(errno));
            if (fds[0] >= 0) {
                close(fds[0]);
                close(fds[1]);
            }
            break;
        }

        if (pid == 0) {
            /* Child: die on Ctrl-C again (the shell ignores it). */
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            /* Wire the pipeline first, explicit redirects second. */
            if (prev_read >= 0) {
                dup2(prev_read, STDIN_FILENO);
                close(prev_read);
            }
            if (c->next) {
                close(fds[0]); /* the reader is the NEXT stage, not us */
                dup2(fds[1], STDOUT_FILENO);
                close(fds[1]);
            }
            if (apply_redirs(c) < 0)
                _exit(1);

            if (c->argc == 0)
                _exit(0); /* redirect-only command: `> file` */

            psh_builtin_fn fn = psh_find_builtin(c->argv[0]);
            if (fn)
                _exit(fn(c->argv) & 0xff);

            execvp(c->argv[0], c->argv);
            if (errno == ENOENT)
                fprintf(stderr, "psh: %s: command not found — %s\n",
                        c->argv[0], psh_pistachio_notfound());
            else
                fprintf(stderr, "psh: %s: %s\n",
                        c->argv[0], strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }

        /*
         * Parent bookkeeping. Closing our copies of the pipe ends is
         * not optional tidiness: a reader only sees EOF once EVERY
         * write end is closed. Leak one here and `a | b` hangs forever
         * with b waiting for input that will never finish.
         */
        pids[spawned++] = pid;
        if (prev_read >= 0)
            close(prev_read);
        if (c->next) {
            close(fds[1]);
            prev_read = fds[0];
        }
    }
    if (prev_read >= 0)
        close(prev_read); /* if the loop broke early */

    int status = (spawned == nstages) ? 0 : 1;
    for (size_t i = 0; i < spawned; i++) {
        int wstatus;
        if (waitpid(pids[i], &wstatus, 0) < 0)
            continue;
        if (i == spawned - 1 && spawned == nstages)
            status = wait_status_to_exit(wstatus);
        else if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGINT)
            fputc('\n', stdout);
    }
    free(pids);
    return status;
}

int psh_execute(psh_stmt *stmts)
{
    int status = 0;
    for (psh_stmt *s = stmts; s; s = s->next)
        status = run_pipeline(s->pipeline);
    return status;
}
