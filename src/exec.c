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
 *   M5: the tree grew if/while/for/funcdef nodes; this file walks
 *       them. Control flow (break/continue/return) travels as a flag
 *       the builtins raise and the loop/function executors consume —
 *       no longjmp gymnastics needed. Functions live in a name→body
 *       table; calling one runs its body with $1..$9 swapped in.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "psh.h"

psh_flow_t psh_flow = PSH_FLOW_NONE;
bool psh_errexit = false;

/* Depth of "tested" contexts (if/while conditions), where a failure
 * is information, not an emergency — set -e must not fire there. */
static int cond_depth;

static int run_stmts(psh_stmt *s);

/* ---------------- functions ---------------- */

typedef struct fentry {
    char *name;
    psh_stmt *body;
    struct fentry *next;
} fentry;

static fentry *functions;

/* Redefining a function that is CURRENTLY EXECUTING must not free
 * its body — the interpreter is still walking it (`omp reload`
 * sources omp.psh, which redefines `omp` while `omp` runs). Old
 * bodies are parked here while any function call is on the stack
 * and swept once the stack is empty again. */
typedef struct zombie {
    psh_stmt *body;
    struct zombie *next;
} zombie;
static zombie *graveyard;
static int func_depth;

static void graveyard_sweep(void)
{
    if (func_depth)
        return;
    while (graveyard) {
        zombie *z = graveyard;
        graveyard = z->next;
        psh_stmts_free(z->body);
        free(z);
    }
}

static psh_stmt *func_lookup(const char *name)
{
    for (fentry *f = functions; f; f = f->next)
        if (strcmp(f->name, name) == 0)
            return f->body;
    return NULL;
}

bool psh_function_exists(const char *name)
{
    return func_lookup(name) != NULL;
}

static void func_define(const char *name, psh_stmt *body)
{
    for (fentry *f = functions; f; f = f->next) {
        if (strcmp(f->name, name) == 0) {
            /* Redefinition replaces — but the old body may be the
             * very code we're executing right now. Park it. */
            zombie *z = func_depth ? malloc(sizeof *z) : NULL;
            if (z) {
                z->body = f->body;
                z->next = graveyard;
                graveyard = z;
            } else if (!func_depth) {
                psh_stmts_free(f->body);
            } /* malloc failed mid-call: leak beats a crash */
            f->body = body;
            return;
        }
    }
    fentry *f = malloc(sizeof *f);
    if (!f) {
        psh_stmts_free(body);
        return;
    }
    f->name = strdup(name);
    f->body = body;
    f->next = functions;
    functions = f;
}

/* Run a function body with $1..$9/$# pointing at ITS arguments;
 * restore the caller's afterwards (so recursion just works). A
 * `return` raises PSH_FLOW_RETURN, which stops the body's statement
 * walk and is absorbed here — the function boundary. */
static int call_function(psh_stmt *body, char **argv)
{
    char **saved_args = psh_script_args;
    size_t saved_argc = psh_script_argc;
    size_t n = 0;
    while (argv[n])
        n++;
    psh_script_args = argv + 1;
    psh_script_argc = n ? n - 1 : 0;
    psh_vars_push_scope(); /* a home for this call's `local`s */
    func_depth++;

    int status = run_stmts(body);
    if (psh_flow == PSH_FLOW_RETURN)
        psh_flow = PSH_FLOW_NONE;

    func_depth--;
    graveyard_sweep(); /* bodies our redefinitions orphaned */
    psh_vars_pop_scope(); /* locals die; shadowed environ restored */
    psh_script_args = saved_args;
    psh_script_argc = saved_argc;
    return status;
}

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
 * (so A=$HOME/src works) but never globbed or split. In the parent,
 * assignments go through the variable table; in a forked child about
 * to exec (`A=1 cmd`), straight into the environ — that process's
 * whole world is the environment, and it dies with the variable. */
static void apply_assignments(const psh_command *c, size_t n, bool to_env)
{
    for (size_t i = 0; i < n; i++) {
        const char *eq = strchr(c->argv[i], '=');
        char *name = strndup(c->argv[i], (size_t)(eq - c->argv[i]));
        char *val = psh_expand_word_single(eq + 1);
        if (name && val) {
            if (to_env)
                setenv(name, val, 1);
            else
                psh_var_set(name, val);
        }
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
 * Apply one command's redirections, IN SOURCE ORDER — the order is
 * the semantics: `>f 2>&1` first points 1 at f, then points 2 at
 * what 1 now is; swapped, 2 goes to the OLD stdout. Targets are
 * expanded here (so `> $LOG`, `2>&$FD` work) but never globbed.
 * Explicit redirects run AFTER pipe wiring, so `a | b > f` sends b's
 * output to f — like sh.
 */
/*
 * H7.3: /dev/tcp/HOST/PORT and /dev/udp/HOST/PORT are not files —
 * they are an open(2)-shaped door to a socket, as in bash (Linux
 * has no such paths; the shell fakes them). PORT may be a service
 * name; HOST may be a numeric IPv6 address (the LAST slash splits).
 * connect() stays interruptible: Ctrl-C aborts a dead host instead
 * of hanging the prompt.
 */
static int open_net_fd(const char *path)
{
    bool udp = strncmp(path, "/dev/udp/", 9) == 0;
    const char *rest = path + 9; /* both prefixes are 9 bytes */
    const char *slash = strrchr(rest, '/');
    if (!slash || slash == rest || !slash[1]) {
        fprintf(stderr, "psh: %s: expected /dev/%s/HOST/PORT\n", path,
                udp ? "udp" : "tcp");
        return -1;
    }
    char *host = strndup(rest, (size_t)(slash - rest));
    if (!host) {
        fprintf(stderr, "psh: out of memory\n");
        return -1;
    }

    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    int gai = getaddrinfo(host, slash + 1, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "psh: %s: %s\n", path,
                gai == EAI_SYSTEM ? strerror(errno) : gai_strerror(gai));
        free(host);
        return -1;
    }
    free(host);

    int fd = -1, cerr = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            cerr = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        cerr = errno;
        close(fd);
        fd = -1;
        if (cerr == EINTR)
            break; /* Ctrl-C mid-connect: stop trying, not the shell */
    }
    freeaddrinfo(res);
    if (fd < 0)
        fprintf(stderr, "psh: %s: %s\n", path,
                cerr == EINTR ? "interrupted" : strerror(cerr));
    return fd;
}

static bool is_net_path(const char *w)
{
    return strncmp(w, "/dev/tcp/", 9) == 0 ||
           strncmp(w, "/dev/udp/", 9) == 0;
}

static int apply_one_redir(const psh_redir *r)
{
    char *word = psh_expand_word_single(r->target);
    if (!word) {
        fprintf(stderr, "psh: out of memory\n");
        return -1;
    }

    if (r->kind == RD_DUPIN || r->kind == RD_DUPOUT) {
        /* [n]>&m duplicates, [n]>&- closes. */
        if (strcmp(word, "-") == 0) {
            close(r->fd);
            free(word);
            return 0;
        }
        char *end;
        long m = strtol(word, &end, 10);
        if (end == word || *end || m < 0 || m > 1000000) {
            fprintf(stderr, "psh: %s: bad file descriptor\n", word);
            free(word);
            return -1;
        }
        free(word);
        if ((int)m != r->fd && dup2((int)m, r->fd) < 0) {
            fprintf(stderr, "psh: %ld: %s\n", m, strerror(errno));
            return -1;
        }
        return 0;
    }

    int fd;
    if (is_net_path(word)) {
        fd = open_net_fd(word); /* prints its own error */
        if (fd < 0) {
            free(word);
            return -1;
        }
    } else {
        int flags = 0;
        switch (r->kind) {
        case RD_IN:     flags = O_RDONLY; break;
        case RD_OUT:    flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case RD_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; break;
        case RD_RDWR:   flags = O_RDWR | O_CREAT; break;
        default: break; /* dup kinds handled above */
        }
        fd = open(word, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "psh: %s: %s\n", word, strerror(errno));
            free(word);
            return -1;
        }
    }
    free(word);
    if (fd != r->fd) {
        if (dup2(fd, r->fd) < 0) {
            fprintf(stderr, "psh: %d: %s\n", r->fd, strerror(errno));
            close(fd);
            return -1;
        }
        close(fd);
    }
    return 0;
}

static int apply_redirs(const psh_command *c)
{
    for (const psh_redir *r = c->redirs; r; r = r->next)
        if (apply_one_redir(r) < 0)
            return -1;
    return 0;
}

/*
 * A lone builtin or function call must run in the shell process
 * (that's what makes it able to cd, set flow flags, define things) —
 * so to honor `pwd > file` we temporarily rewire the SHELL'S own fds
 * and put them back afterwards. Exactly one of fn/funcbody is set.
 *
 * Every fd a redirection touches is parked as a CLOEXEC copy at
 * 10+ first (out of the low userland range, bash's trick). A save
 * that fails with EBADF means the fd was CLOSED before — restoring
 * it means closing it again. Saves run in source order and restores
 * in reverse, so `>a >b` (fd 1 touched twice) unwinds correctly.
 */
static int run_builtin_in_parent(psh_builtin_fn fn, psh_stmt *funcbody,
                                 const psh_command *c, char **fargv)
{
    size_t nredir = 0;
    for (const psh_redir *r = c->redirs; r; r = r->next)
        nredir++;

    typedef struct { int fd; int saved; } fdsave;
    fdsave *saves = NULL;
    if (nredir) {
        saves = malloc(nredir * sizeof *saves);
        if (!saves) {
            fprintf(stderr, "psh: out of memory\n");
            return 1;
        }
        /* Park above every fd the redirs touch, so a copy can never
         * land on a number a later redirection is about to rewire. */
        int floor = 10;
        for (const psh_redir *r = c->redirs; r; r = r->next)
            if (r->fd >= floor)
                floor = r->fd + 1;
        size_t i = 0;
        for (const psh_redir *r = c->redirs; r; r = r->next, i++) {
            saves[i].fd = r->fd;
            saves[i].saved = fcntl(r->fd, F_DUPFD_CLOEXEC, floor);
        }
    }

    int status;
    if (apply_redirs(c) < 0)
        status = 1;
    else
        status = fn ? fn(fargv) : call_function(funcbody, fargv);

    /* Flush BEFORE restoring, or buffered output lands on the old fd. */
    fflush(stdout);
    fflush(stderr);
    for (size_t i = nredir; i-- > 0;) {
        if (saves[i].saved >= 0) {
            dup2(saves[i].saved, saves[i].fd);
            close(saves[i].saved);
        } else {
            close(saves[i].fd);
        }
    }
    free(saves);
    return status;
}

/*
 * Single-stage fast paths that must NOT fork: pure assignments and
 * builtins. Returns the status, or -1 meaning "use the fork path" —
 * and in that case *out_fargv hands the ALREADY-EXPANDED argv to the
 * caller, because expanding twice would run $( ) substitutions (and
 * their side effects) twice. Found the hard way: a ${bad@} warning
 * printed two times for external commands, once per expansion.
 */
static int try_run_in_parent(const psh_command *c, char ***out_fargv)
{
    size_t na = count_leading_assignments(c);
    *out_fargv = NULL;

    /* Pure assignment(s), no command, no redirects: set and done. */
    if (na == c->argc && c->argc > 0 && !c->redirs) {
        apply_assignments(c, na, false);
        return 0;
    }
    if (na == c->argc)
        return -1; /* redirect-only (or with redirects): fork path */

    char **fargv = expand_command_argv(c, na);
    if (!fargv)
        return 1;
    if (!fargv[0]) { /* every word expanded away: nothing to run */
        free_strv(fargv);
        apply_assignments(c, na, false);
        return 0;
    }

    /* exec is SPECIAL (POSIX): found before functions, never
     * forked. Its redirections are applied to the SHELL and stay —
     * `exec 3<>file` gives every later command fd 3. With a
     * command, this shell's story ends here: execvp in place (a
     * file from $PATH only — exec doesn't run builtins). A failed
     * exec is reported but survived, like interactive bash. */
    if (strcmp(fargv[0], "exec") == 0) {
        apply_assignments(c, na, false);
        int status = 0;
        if (apply_redirs(c) < 0) {
            status = 1;
        } else if (fargv[1]) {
            /* Ignored dispositions SURVIVE execvp; hand the program
             * default ones (handlers reset themselves), remembering
             * the old ones in case the exec fails and we are still
             * a shell after all. */
            static const int sigs[] = { SIGQUIT, SIGTSTP, SIGTTIN,
                                        SIGTTOU, SIGPIPE };
            enum { NSIGS = sizeof sigs / sizeof sigs[0] };
            void (*old[NSIGS])(int);
            for (size_t i = 0; i < NSIGS; i++)
                old[i] = signal(sigs[i], SIG_DFL);
            execvp(fargv[1], fargv + 1);
            status = errno == ENOENT ? 127 : 126;
            fprintf(stderr, "psh: exec: %s: %s\n", fargv[1],
                    errno == ENOENT ? "command not found"
                                    : strerror(errno));
            for (size_t i = 0; i < NSIGS; i++)
                signal(sigs[i], old[i]);
        }
        free_strv(fargv);
        return status;
    }

    /* Functions shadow builtins, builtins shadow $PATH — sh's order. */
    psh_stmt *funcbody = func_lookup(fargv[0]);
    psh_builtin_fn fn = funcbody ? NULL : psh_find_builtin(fargv[0]);
    if (!fn && !funcbody) {
        *out_fargv = fargv; /* the fork path takes it, expanded once */
        return -1;
    }
    apply_assignments(c, na, false); /* `A=1 cd /x`: rare, but honor it */
    int status = run_builtin_in_parent(fn, funcbody, c, fargv);
    free_strv(fargv);
    return status;
}

static int run_pipeline(const psh_command *first, bool background)
{
    char **preexpanded = NULL;
    if (!background && !first->next) {
        int status = try_run_in_parent(first, &preexpanded);
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
        char **fargv = preexpanded ? preexpanded
                                   : expand_command_argv(c, na);
        preexpanded = NULL; /* only ever the first (only) stage's */
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
            apply_assignments(c, na, true);

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

            /* `exec cmd` in a pipeline: the child just becomes it —
             * no function/builtin lookup, files only, like sh. */
            if (strcmp(fargv[0], "exec") == 0) {
                if (!fargv[1])
                    _exit(0); /* its redirs applied above; done */
                fargv++;
            } else {
                psh_stmt *funcbody = func_lookup(fargv[0]);
                if (funcbody) /* function in a pipeline: runs here */
                    _exit(call_function(funcbody, fargv) & 0xff);
                psh_builtin_fn fn = psh_find_builtin(fargv[0]);
                if (fn)
                    _exit(fn(fargv) & 0xff);
            }

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

static int run_stmt_foreground(psh_stmt *s);

/*
 * `cmd &` backgrounds a single pipeline directly. Anything bigger —
 * an &&/|| list, a loop, an if — runs inside one forked subshell,
 * which is also exactly what bash does.
 */
static int run_stmt_background(psh_stmt *s)
{
    if (s->kind == ST_LIST && s->list->conn == PSH_CONN_END)
        return run_pipeline(s->list->pipeline, true);

    char *desc = s->kind == ST_LIST ? andor_to_string(s->list)
                                    : strdup("(background job)");
    psh_job *job = psh_job_create(desc);
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
        _exit(run_stmt_foreground(s) & 0xff);
    }
    psh_job_set_pgid(job, pid);
    if (psh_job_control)
        setpgid(pid, pid);
    psh_job_add_pid(job, pid);
    psh_job_background(job);
    return 0;
}

static int run_stmt_foreground(psh_stmt *s)
{
    switch (s->kind) {

    case ST_LIST:
        return run_andor(s->list);

    case ST_IF: {
        cond_depth++;
        int c = run_stmts(s->cond);
        cond_depth--;
        if (psh_flow != PSH_FLOW_NONE)
            return c;
        if (c == 0)
            return run_stmts(s->body);
        return s->else_body ? run_stmts(s->else_body) : 0;
    }

    case ST_WHILE: {
        int status = 0;
        for (;;) {
            if (psh_interrupted)
                return 130;
            cond_depth++;
            int c = run_stmts(s->cond);
            cond_depth--;
            if (psh_flow != PSH_FLOW_NONE || c != 0)
                break;
            status = run_stmts(s->body);
            if (psh_flow == PSH_FLOW_BREAK) {
                psh_flow = PSH_FLOW_NONE;
                break;
            }
            if (psh_flow == PSH_FLOW_CONTINUE)
                psh_flow = PSH_FLOW_NONE;
            else if (psh_flow == PSH_FLOW_RETURN)
                break; /* propagate up to the function boundary */
        }
        return status;
    }

    case ST_FOR: {
        int status = 0;
        bool stop = false;
        for (size_t i = 0; i < s->nwords && !stop; i++) {
            size_t k;
            char **vals = psh_expand_word(s->words[i], &k);
            if (!vals)
                return 1;
            for (size_t j = 0; j < k && !stop; j++) {
                if (psh_interrupted) {
                    status = 130;
                    stop = true;
                    break;
                }
                psh_var_set(s->name, vals[j]);
                status = run_stmts(s->body);
                if (psh_flow == PSH_FLOW_BREAK) {
                    psh_flow = PSH_FLOW_NONE;
                    stop = true;
                } else if (psh_flow == PSH_FLOW_CONTINUE) {
                    psh_flow = PSH_FLOW_NONE;
                } else if (psh_flow == PSH_FLOW_RETURN) {
                    stop = true;
                }
            }
            free_strv(vals);
        }
        return status;
    }

    case ST_CASE: {
        char *subject = psh_expand_word_single(s->name);
        if (!subject)
            return 1;
        int status = 0;
        for (psh_case_item *item = s->case_items; item;
             item = item->next) {
            bool matched = false;
            for (size_t i = 0; i < item->npatterns && !matched; i++) {
                char *pat = psh_expand_word_single(item->patterns[i]);
                if (pat && fnmatch(pat, subject, 0) == 0)
                    matched = true;
                free(pat);
            }
            if (matched) {
                status = item->body ? run_stmts(item->body) : 0;
                break; /* first match wins; no ;& fallthrough */
            }
        }
        free(subject);
        return status;
    }

    case ST_FUNCDEF:
        /* Steal the body subtree from the AST — the definition
         * outlives this line, the rest of the tree doesn't. */
        func_define(s->name, s->body);
        s->body = NULL;
        return 0;
    }
    return 0;
}

static int run_stmts(psh_stmt *s)
{
    int status = psh_last_status;
    for (; s; s = s->next) {
        if (psh_interrupted) {
            status = 130;
            break;
        }
        status = s->background ? run_stmt_background(s)
                               : run_stmt_foreground(s);
        psh_last_status = status;
        if (psh_flow != PSH_FLOW_NONE)
            break; /* break/continue/return bubbles up */
        /* set -e: an UNTESTED single-pipeline failure ends the shell.
         * Tested places don't count: if/while conditions, and lists
         * where && or || already inspects the status. */
        if (psh_errexit && status != 0 && cond_depth == 0 &&
            s->kind == ST_LIST && !s->background &&
            s->list->conn == PSH_CONN_END)
            exit(status);
    }
    return status;
}

int psh_execute(psh_stmt *stmts)
{
    return run_stmts(stmts);
}
