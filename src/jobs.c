/*
 * jobs.c — job control: the shell's hardest classic-Unix material.
 *
 * The mental model, in four sentences:
 *
 *   1. A JOB is one pipeline; all its processes share one PROCESS
 *      GROUP so they can be signaled as a unit (kill(-pgid, ...)).
 *   2. The terminal has exactly one FOREGROUND process group at a
 *      time (tcsetpgrp); the kernel delivers Ctrl-C's SIGINT and
 *      Ctrl-Z's SIGTSTP to that group and nobody else. That single
 *      mechanism is all of Ctrl-Z.
 *   3. The shell parks itself in its own group, ignores the stop
 *      signals, and hands the terminal to whichever job is
 *      foreground — taking it back when the job exits or stops.
 *   4. waitpid(..., WUNTRACED) reports "stopped" as well as "died",
 *      which is how a Ctrl-Z'd job lands in the jobs table instead
 *      of being mourned.
 *
 * Everything else here is bookkeeping around those four facts.
 * (Reference: the GNU libc manual's "Implementing a Job Control
 * Shell" chapter — the canonical walkthrough of this dance.)
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "psh.h"

bool psh_job_control = false;

#define TTY STDIN_FILENO

static pid_t shell_pgid;
static struct termios shell_tmodes;

typedef enum { J_RUNNING, J_STOPPED, J_DONE } jstate;

struct psh_job {
    int id;        /* the [1] in "[1] 4242" */
    pid_t pgid;    /* process group = pid of the first child */
    char *cmdline; /* what to show in `jobs` */
    pid_t *pids;
    size_t npids, cap, ndone;
    jstate state;
    int last_status; /* exit status of the LAST pid = the job's status */
    int stopsig;
    bool saw_sigint;
    bool stop_reported;
    struct termios tmodes; /* terminal state saved when the job stops */
    bool has_tmodes;
    struct psh_job *next;
};

static psh_job *jobs_head;

/* ---------------- table bookkeeping ---------------- */

psh_job *psh_job_create(char *cmdline)
{
    psh_job *j = calloc(1, sizeof *j);
    if (!j) {
        free(cmdline);
        return NULL;
    }
    j->cmdline = cmdline ? cmdline : strdup("(job)");
    j->state = J_RUNNING;
    int maxid = 0;
    psh_job **tail = &jobs_head;
    for (psh_job *it = jobs_head; it; it = it->next) {
        if (it->id > maxid)
            maxid = it->id;
        tail = &it->next;
    }
    j->id = maxid + 1;
    *tail = j; /* newest at the tail = the "current" job for fg/bg */
    return j;
}

static void job_remove(psh_job *j)
{
    for (psh_job **it = &jobs_head; *it; it = &(*it)->next) {
        if (*it == j) {
            *it = j->next;
            free(j->pids);
            free(j->cmdline);
            free(j);
            return;
        }
    }
}

void psh_job_discard(psh_job *j)
{
    if (j)
        job_remove(j);
}

void psh_job_add_pid(psh_job *j, pid_t pid)
{
    if (j->npids + 1 > j->cap) {
        size_t cap = j->cap ? j->cap * 2 : 8;
        pid_t *grown = realloc(j->pids, cap * sizeof *grown);
        if (!grown)
            return;
        j->pids = grown;
        j->cap = cap;
    }
    j->pids[j->npids++] = pid;
}

size_t psh_job_npids(const psh_job *j) { return j->npids; }
pid_t psh_job_get_pgid(const psh_job *j) { return j->pgid; }
void psh_job_set_pgid(psh_job *j, pid_t pgid) { j->pgid = pgid; }

/* ---------------- reaping ---------------- */

/* Record what waitpid told us about one pid, whichever job owns it.
 * This dispatch-by-lookup is what lets any wait loop reap ANY child
 * (foreground wait can collect a background job's corpse in passing). */
static void mark_pid(pid_t pid, int wstatus)
{
    for (psh_job *j = jobs_head; j; j = j->next) {
        for (size_t i = 0; i < j->npids; i++) {
            if (j->pids[i] != pid)
                continue;
            if (WIFSTOPPED(wstatus)) {
                j->state = J_STOPPED;
                j->stopsig = WSTOPSIG(wstatus);
                j->stop_reported = false;
                return;
            }
            j->ndone++;
            if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGINT)
                j->saw_sigint = true;
            if (i == j->npids - 1)
                j->last_status = WIFEXITED(wstatus)
                                     ? WEXITSTATUS(wstatus)
                                     : 128 + WTERMSIG(wstatus);
            if (j->ndone == j->npids)
                j->state = J_DONE;
            return;
        }
    }
    /* not ours (shouldn't happen) — silently ignore */
}

void psh_jobs_reap(void)
{
    int ws;
    pid_t pid;
    while ((pid = waitpid(-1, &ws,
                          WNOHANG | (psh_job_control ? WUNTRACED : 0))) > 0)
        mark_pid(pid, ws);
}

static void purge_done(bool report)
{
    psh_job *j = jobs_head;
    while (j) {
        psh_job *next = j->next;
        if (j->state == J_DONE) {
            if (report)
                printf("[%d]+  Done\t%s\n", j->id, j->cmdline);
            job_remove(j);
        }
        j = next;
    }
}

/* Called before every prompt: deliver the news since last time. */
void psh_jobs_notify(void)
{
    psh_jobs_reap();
    for (psh_job *j = jobs_head; j; j = j->next) {
        if (j->state == J_STOPPED && !j->stop_reported) {
            printf("[%d]+  Stopped\t%s\n", j->id, j->cmdline);
            j->stop_reported = true;
        }
    }
    purge_done(true);
}

/* ---------------- setup ---------------- */

void psh_jobs_init(bool interactive)
{
    psh_job_control = interactive && isatty(TTY);
    if (!psh_job_control)
        return;

    /* If psh itself was started in the background, politely stop
     * until we're given the terminal — grabbing it from the real
     * foreground process would be rude and undefined. */
    while (tcgetpgrp(TTY) != getpgrp())
        kill(-getpgrp(), SIGTTIN);

    /* The stop signals must never stop the SHELL — only its jobs.
     * (SIGINT/SIGQUIT are already ignored in main.) */
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    /* Our own private process group, owning the terminal. */
    shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid); /* EPERM if session leader: fine */
    tcsetpgrp(TTY, shell_pgid);
    tcgetattr(TTY, &shell_tmodes);
}

/*
 * Runs in the freshly forked child, before exec. pgid is the job's
 * group as known at fork time — 0 means "I am the first: found the
 * group". Both child and parent call setpgid with the same values;
 * whichever runs first wins and the race disappears (the classic
 * both-sides trick from the glibc manual).
 */
void psh_job_child_setup(pid_t pgid, bool foreground)
{
    if (psh_job_control) {
        pid_t self = getpid();
        if (pgid == 0)
            pgid = self;
        setpgid(self, pgid);
        if (foreground)
            tcsetpgrp(TTY, pgid);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
    }
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL); /* the shell may ignore it; jobs mustn't */
}

/* ---------------- foreground / background ---------------- */

int psh_job_foreground(psh_job *j, bool cont)
{
    j->state = J_RUNNING;
    if (psh_job_control) {
        tcsetpgrp(TTY, j->pgid); /* the job owns the terminal now */
        if (cont) {
            /* Resuming a stopped job: give it back the terminal
             * state it was stopped with (think: vi's raw mode). */
            if (j->has_tmodes)
                tcsetattr(TTY, TCSADRAIN, &j->tmodes);
            kill(-j->pgid, SIGCONT);
        }
    } else if (cont) {
        kill(-j->pgid, SIGCONT);
    }

    while (j->state == J_RUNNING) {
        int ws;
        pid_t pid = waitpid(-1, &ws, psh_job_control ? WUNTRACED : 0);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            j->state = J_DONE; /* ECHILD: nothing left to wait for */
            break;
        }
        mark_pid(pid, ws);
    }

    int status;
    if (j->state == J_STOPPED) {
        status = 128 + j->stopsig; /* bash convention for a stop */
        if (psh_job_control)
            j->has_tmodes = (tcgetattr(TTY, &j->tmodes) == 0);
        printf("\n[%d]+  Stopped\t%s\n", j->id, j->cmdline);
        j->stop_reported = true;
        /* the job STAYS in the table — that's the whole point */
    } else {
        if (j->saw_sigint)
            fputc('\n', stdout); /* land the prompt on a fresh line */
        status = j->last_status;
        job_remove(j);
    }

    if (psh_job_control) {
        /* Take the terminal back and restore OUR settings — the job
         * may have left it in raw mode. */
        tcsetpgrp(TTY, shell_pgid);
        tcsetattr(TTY, TCSADRAIN, &shell_tmodes);
    }
    return status;
}

void psh_job_background(psh_job *j)
{
    j->state = J_RUNNING;
    if (psh_job_control)
        printf("[%d] %d\n", j->id, (int)j->pgid);
}

/* ---------------- the builtins ---------------- */

static psh_job *find_job(const char *spec)
{
    if (!spec) { /* no argument: the current (most recent) job */
        psh_job *last = NULL;
        for (psh_job *j = jobs_head; j; j = j->next)
            last = j;
        return last;
    }
    if (*spec == '%')
        spec++;
    int id = atoi(spec);
    for (psh_job *j = jobs_head; j; j = j->next)
        if (j->id == id)
            return j;
    return NULL;
}

int psh_builtin_jobs(char **argv)
{
    (void)argv;
    psh_jobs_reap();
    psh_job *current = find_job(NULL);
    for (psh_job *j = jobs_head; j; j = j->next) {
        const char *state = j->state == J_STOPPED ? "Stopped"
                          : j->state == J_DONE   ? "Done"
                                                 : "Running";
        printf("[%d]%c  %-8s\t%s%s\n", j->id, j == current ? '+' : ' ',
               state, j->cmdline,
               j->state == J_RUNNING ? " &" : "");
    }
    purge_done(false);
    return 0;
}

int psh_builtin_fg(char **argv)
{
    if (!psh_job_control) {
        fprintf(stderr, "psh: fg: no job control in this shell\n");
        return 1;
    }
    psh_jobs_reap();
    psh_job *j = find_job(argv[1]);
    if (!j || j->state == J_DONE) {
        fprintf(stderr, "psh: fg: %s: no such job\n",
                argv[1] ? argv[1] : "current");
        return 1;
    }
    printf("%s\n", j->cmdline);
    return psh_job_foreground(j, true);
}

int psh_builtin_bg(char **argv)
{
    if (!psh_job_control) {
        fprintf(stderr, "psh: bg: no job control in this shell\n");
        return 1;
    }
    psh_jobs_reap();
    psh_job *j = find_job(argv[1]);
    if (!j || j->state == J_DONE) {
        fprintf(stderr, "psh: bg: %s: no such job\n",
                argv[1] ? argv[1] : "current");
        return 1;
    }
    if (j->state == J_RUNNING) {
        fprintf(stderr, "psh: bg: job %d already running\n", j->id);
        return 0;
    }
    j->state = J_RUNNING;
    printf("[%d]+ %s &\n", j->id, j->cmdline);
    kill(-j->pgid, SIGCONT);
    return 0;
}

int psh_builtin_wait(char **argv)
{
    (void)argv;
    for (psh_job *j = jobs_head; j; j = j->next) {
        while (j->state == J_RUNNING) {
            int ws;
            pid_t pid = waitpid(-1, &ws, psh_job_control ? WUNTRACED : 0);
            if (pid < 0) {
                if (errno == EINTR)
                    continue;
                j->state = J_DONE;
                break;
            }
            mark_pid(pid, ws);
        }
    }
    purge_done(false);
    return 0;
}
