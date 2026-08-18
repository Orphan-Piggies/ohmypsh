/*
 * exec.c — run a syntax tree.
 *
 * The core is unchanged since M2: a pipeline is a loop of
 * pipe() + fork() + dup2() + execvp(), all stages concurrent, exit
 * status taken from the last stage. What changed per milestone:
 *
 *   M3: &&/|| lists, execution-time expansion, NAME=value assignments.
 *   M4: every pipeline is now a JOB (see jobs.c). Foreground jobs own
 *       the terminal while they run; `cmd &` skips the wait entirely;
 *       a whole `a && b &` list runs inside one forked subshell.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "psh.h"

static void free_strv(char **v)
{
    if (!v)
        return;
    for (size_t i = 0; v[i]; i++)
        free(v[i]);
    free(v);
}

/* Is this raw word a NAME=... assignment? (A quoted name like "A"=1
 * fails the name check and stays an ordinary word, as in sh.) */
static bool is_assignment(const char *raw)
{
    if (!isalpha((unsigned char)raw[0]) && raw[0] != '_')
        return false;
    size_t i = 1;
    while (isalnum((unsigned char)raw[i]) || raw[i] == '_')
        i++;
    return raw[i] == '=';
}

static size_t count_leading_assignments(const psh_command *c)
{
    size_t n = 0;
    while (n < c->argc && is_assignment(c->argv[n]))
        n++;
    return n;
}

/* Apply the first n assignment words. The VALUE side is expanded
 * (so A=$HOME/src works) but never globbed or split. */
static void apply_assignments(const psh_command *c, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char *eq = strchr(c->argv[i], '=');
        char *name = strndup(c->argv[i], (size_t)(eq - c->argv[i]));
        char *val = psh_expand_word_single(eq + 1);
        if (name && val)
            setenv(name, val, 1);
        free(name);
        free(val);
    }
}

/* Expand every argv word after the leading assignments into the final
 * NULL-terminated vector the command will receive. Globs can fan one
 * word into many; empty unquoted expansions vanish. */
static char **expand_command_argv(const psh_command *c, size_t skip)
{
    size_t cap = c->argc + 1, n = 0;
    char **out = malloc(cap * sizeof *out);
    if (!out)
        return NULL;

    for (size_t i = skip; i < c->argc; i++) {
        size_t k;
        char **words = psh_expand_word(c->argv[i], &k);
        if (!words) {
            out[n] = NULL;
            free_strv(out);
            return NULL;
        }
        if (n + k + 1 > cap) {
            cap = (n + k + 1) * 2;
            char **grown = realloc(out, cap * sizeof *out);
            if (!grown) {
                free_strv(words);
                out[n] = NULL;
                free_strv(out);
                return NULL;
            }
            out = grown;
        }
        for (size_t j = 0; j < k; j++)
            out[n++] = words[j];
        free(words); /* container only; the strings moved into out */
    }
    out[n] = NULL;
    return out;
}

/* The raw command text, rebuilt for `jobs` listings. */
static char *pipeline_to_string(const psh_command *first)
{
    size_t len = 1;
    for (const psh_command *c = first; c; c = c->next) {
        for (size_t i = 0; i < c->argc; i++)
            len += strlen(c->argv[i]) + 1;
        len += 3;
    }
    char *s = malloc(len);
    if (!s)
        return NULL;
    s[0] = '\0';
    for (const psh_command *c = first; c; c = c->next) {
        for (size_t i = 0; i < c->argc; i++) {
            strcat(s, c->argv[i]);
            if (i + 1 < c->argc)
                strcat(s, " ");
        }
        if (c->next)
            strcat(s, " | ");
    }
    return s;
}

static char *andor_to_string(const psh_andor *list)
{
    char *s = pipeline_to_string(list->pipeline);
    for (const psh_andor *a = list; s && a->conn != PSH_CONN_END;) {
        const char *sym = a->conn == PSH_CONN_AND ? " && " : " || ";
        a = a->next;
        char *piece = pipeline_to_string(a->pipeline);
        if (!piece)
            break;
        char *grown = realloc(s, strlen(s) + strlen(sym) + strlen(piece) + 1);
        if (!grown) {
            free(piece);
            break;
        }
        s = grown;
        strcat(s, sym);
        strcat(s, piece);
        free(piece);
    }
    return s;
}

/*
 * Splice this command's redirect files onto fd 0/1/2. Paths are
 * expanded here (so `> $LOG` and `< ~/notes` work) but never globbed.
 * Explicit redirects run AFTER pipe wiring, so `a | b > f` sends b's
 * output to f — like sh.
 */
static int apply_one_redir(const char *raw_path, int flags, int target_fd)
{
    char *path = psh_expand_word_single(raw_path);
    if (!path) {
        fprintf(stderr, "psh: out of memory\n");
        return -1;
    }
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "psh: %s: %s\n", path, strerror(errno));
        free(path);
        return -1;
    }
    free(path);
    dup2(fd, target_fd);
    close(fd);
    return 0;
}

static int apply_redirs(const psh_command *c)
{
    if (c->in_path &&
        apply_one_redir(c->in_path, O_RDONLY, STDIN_FILENO) < 0)
        return -1;
    if (c->out_path) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        if (apply_one_redir(c->out_path, flags, STDOUT_FILENO) < 0)
            return -1;
    }
    if (c->err_path &&
        apply_one_redir(c->err_path, O_WRONLY | O_CREAT | O_TRUNC,
                        STDERR_FILENO) < 0)
        return -1;
    return 0;
}

/*
 * A lone builtin must run in the shell process (that's what makes it
 * a builtin) — so to honor `pwd > file` we temporarily rewire the
 * SHELL'S own fds and put them back afterwards.
 */
static int run_builtin_in_parent(psh_builtin_fn fn, const psh_command *c,
                                 char **fargv)
{
    int saved[3] = { -1, -1, -1 };
    if (c->in_path)
        saved[0] = dup(STDIN_FILENO);
    if (c->out_path)
        saved[1] = dup(STDOUT_FILENO);
    if (c->err_path)
        saved[2] = dup(STDERR_FILENO);

    int status = (apply_redirs(c) < 0) ? 1 : fn(fargv);

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

/*
 * Single-stage fast paths that must NOT fork: pure assignments and
 * builtins. Returns the status, or -1 meaning "use the fork path".
 */
static int try_run_in_parent(const psh_command *c)
{
    size_t na = count_leading_assignments(c);

    /* Pure assignment(s), no command, no redirects: set and done. */
    if (na == c->argc && c->argc > 0 && !c->in_path && !c->out_path &&
        !c->err_path) {
        apply_assignments(c, na);
        return 0;
    }
    if (na == c->argc)
        return -1; /* redirect-only (or with redirects): fork path */

    char **fargv = expand_command_argv(c, na);
    if (!fargv)
        return 1;
    if (!fargv[0]) { /* every word expanded away: nothing to run */
        free_strv(fargv);
        apply_assignments(c, na);
        return 0;
    }

    psh_builtin_fn fn = psh_find_builtin(fargv[0]);
    if (!fn) {
        free_strv(fargv);
        return -1;
    }
    apply_assignments(c, na); /* `A=1 cd /x`: rare, but honor it */
    int status = run_builtin_in_parent(fn, c, fargv);
    free_strv(fargv);
    return status;
}

static int run_pipeline(const psh_command *first, bool background)
{
    if (!background && !first->next) {
        int status = try_run_in_parent(first);
        if (status >= 0)
            return status;
    }

    size_t nstages = 0;
    for (const psh_command *c = first; c; c = c->next)
        nstages++;

    psh_job *job = psh_job_create(pipeline_to_string(first));
    if (!job)
        return 1;

    bool incomplete = false;
    int prev_read = -1; /* read end of the pipe from the previous stage */

    for (const psh_command *c = first; c; c = c->next) {
        size_t na = count_leading_assignments(c);
        char **fargv = expand_command_argv(c, na);
        if (!fargv) {
            incomplete = true;
            break;
        }

        int fds[2] = { -1, -1 };
        if (c->next && pipe(fds) < 0) {
            fprintf(stderr, "psh: pipe: %s\n", strerror(errno));
            free_strv(fargv);
            incomplete = true;
            break;
        }

        /* The job's pgid as of THIS fork: 0 for the first stage
         * ("you found the group"), the real pgid afterwards. The
         * child's memory is a snapshot, so pass it explicitly. */
        pid_t pgid_snapshot = psh_job_get_pgid(job);

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "psh: fork: %s\n", strerror(errno));
            free_strv(fargv);
            if (fds[0] >= 0) {
                close(fds[0]);
                close(fds[1]);
            }
            incomplete = true;
            break;
        }

        if (pid == 0) {
            /* Child: join the job's process group, take the terminal
             * if we're the foreground job, restore default signals. */
            psh_job_child_setup(pgid_snapshot, !background);

            /* `A=1 cmd`: the child setenvs, execs, and takes the
             * variable to its grave — per-command env for free. */
            apply_assignments(c, na);

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

            if (!fargv[0])
                _exit(0); /* redirect-only command: `> file` */

            psh_builtin_fn fn = psh_find_builtin(fargv[0]);
            if (fn)
                _exit(fn(fargv) & 0xff);

            execvp(fargv[0], fargv);
            if (errno == ENOENT)
                fprintf(stderr, "psh: %s: command not found — %s\n",
                        fargv[0], psh_pistachio_notfound());
            else
                fprintf(stderr, "psh: %s: %s\n", fargv[0], strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }

        /* Parent: mirror the child's setpgid (whoever runs first
         * wins — that's the point), then the usual pipe hygiene:
         * close our copies or readers never see EOF. */
        if (psh_job_get_pgid(job) == 0)
            psh_job_set_pgid(job, pid);
        if (psh_job_control)
            setpgid(pid, psh_job_get_pgid(job));
        psh_job_add_pid(job, pid);

        free_strv(fargv);
        if (prev_read >= 0)
            close(prev_read);
        if (c->next) {
            close(fds[1]);
            prev_read = fds[0];
        }
    }
    if (prev_read >= 0)
        close(prev_read); /* if the loop broke early */

    if (psh_job_npids(job) == 0) {
        psh_job_discard(job);
        return 1;
    }

    if (background) {
        psh_job_background(job);
        return 0;
    }
    int status = psh_job_foreground(job, false);
    return incomplete ? 1 : status;
}

static int run_andor(psh_andor *list)
{
    psh_andor *a = list;
    int status = run_pipeline(a->pipeline, false);
    psh_last_status = status; /* keep $? live between pipelines */
    while (a->conn != PSH_CONN_END) {
        psh_conn conn = a->conn;
        a = a->next;
        /* Skipped pipelines leave the status untouched — that's
         * what makes `false && a || b` fall through to b. */
        if ((conn == PSH_CONN_AND && status == 0) ||
            (conn == PSH_CONN_OR && status != 0)) {
            status = run_pipeline(a->pipeline, false);
            psh_last_status = status;
        }
    }
    return status;
}

/*
 * `a & ` backgrounds a single pipeline directly. `a && b &` must
 * background the WHOLE list, so it runs inside one forked subshell —
 * which is also exactly what bash does.
 */
static int run_background_stmt(psh_stmt *s)
{
    if (s->list->conn == PSH_CONN_END)
        return run_pipeline(s->list->pipeline, true);

    psh_job *job = psh_job_create(andor_to_string(s->list));
    if (!job)
        return 1;

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "psh: fork: %s\n", strerror(errno));
        psh_job_discard(job);
        return 1;
    }
    if (pid == 0) {
        psh_job_child_setup(0, false);
        psh_job_control = false; /* the subshell plays no terminal games */
        _exit(run_andor(s->list) & 0xff);
    }
    psh_job_set_pgid(job, pid);
    if (psh_job_control)
        setpgid(pid, pid);
    psh_job_add_pid(job, pid);
    psh_job_background(job);
    return 0;
}

int psh_execute(psh_stmt *stmts)
{
    int status = psh_last_status;
    for (psh_stmt *s = stmts; s; s = s->next) {
        status = s->background ? run_background_stmt(s)
                               : run_andor(s->list);
        psh_last_status = status;
    }
    return status;
}
