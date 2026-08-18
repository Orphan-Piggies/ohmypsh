/*
 * psh.h — shared declarations for the pistachio shell.
 *
 * The pipeline of the shell itself (fitting, really):
 *
 *   main.c      the REPL: prompt → read (readline) → run → repeat
 *   lexer.c     raw line → token stream (words kept RAW, quotes intact)
 *   parser.c    tokens → statements → &&/|| lists → pipelines → commands
 *   expand.c    raw word → final strings: $VAR, ~, quote removal, glob
 *   exec.c      runs a syntax tree: builtins, fork/exec, pipes, redirects
 *   builtins.c  commands that must run inside the shell process itself
 *   pistachio.c 🫛 flavor, banners, and easter pistachios
 *
 * The order matters and mirrors real shells: parse FIRST, expand at
 * EXECUTION time, remove quotes LAST. That is why `X=5; echo $X`
 * prints 5 — by the time echo's words expand, the assignment ran.
 */
#ifndef PSH_H
#define PSH_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h> /* pid_t */

#define PSH_VERSION "0.4.0"

/*
 * The mascot glyph, used in the prompt and banners. Unicode has no
 * pistachio emoji (🥜 is officially PEANUTS, U+1F95C — an insult we do
 * not accept), so we use the pea pod: green, lives in a shell, close
 * enough. Rebranding the whole shell is a one-line edit here.
 */
#define PSH_NUT "🫛"

/* Exit status of the last command — what $? expands to and what the
 * prompt shows in red. Defined in main.c. */
extern int psh_last_status;

/* ---------------- tokens (lexer.c) ---------------- */

typedef enum {
    TOK_WORD,
    TOK_PIPE,         /* |  */
    TOK_AND,          /* && */
    TOK_OR,           /* || */
    TOK_SEMI,         /* ;  */
    TOK_AMP,          /* &  — statement terminator meaning "background" */
    TOK_REDIR_IN,     /* <  */
    TOK_REDIR_OUT,    /* >  */
    TOK_REDIR_APPEND, /* >> */
    TOK_REDIR_ERR,    /* 2> */
} psh_token_type;

typedef struct psh_token {
    psh_token_type type;
    char *text; /* TOK_WORD only: the RAW word, quotes still inside */
    struct psh_token *next;
} psh_token;

/* Returns the token list (NULL for a blank line). On a syntax error,
 * prints a message, sets *err, and returns NULL. */
psh_token *psh_tokenize(const char *line, bool *err);
void psh_tokens_free(psh_token *t);

/* ---------------- syntax tree (parser.c) ---------------- */

/* One command: what to run, plus where its three standard streams go.
 * argv and the redirect paths are RAW words; expand.c finishes them at
 * execution time. Repeated redirects of one kind: last wins, like sh. */
typedef struct psh_command {
    char **argv;  /* NULL-terminated raw words */
    size_t argc;
    char *in_path;  /* <  */
    char *out_path; /* > or >> */
    char *err_path; /* 2> */
    bool append;    /* out_path came from >> */
    struct psh_command *next; /* next stage of the pipeline */
} psh_command;

/* How the NEXT element of an &&/|| list is joined to this one. */
typedef enum {
    PSH_CONN_END, /* nothing follows */
    PSH_CONN_AND, /* run next only if this succeeded  (&&) */
    PSH_CONN_OR,  /* run next only if this failed     (||) */
} psh_conn;

typedef struct psh_andor {
    psh_command *pipeline;
    psh_conn conn;
    struct psh_andor *next;
} psh_andor;

/* One statement = one &&/|| list. Statements are separated by ';'
 * or by '&', which additionally marks the statement as background. */
typedef struct psh_stmt {
    psh_andor *list;
    bool background;
    struct psh_stmt *next;
} psh_stmt;

psh_stmt *psh_parse(psh_token *tokens, bool *err);
void psh_stmts_free(psh_stmt *s);

/* ---------------- expansion (expand.c) ---------------- */

/* Raw word → NULL-terminated array of final strings. Globbing can
 * fan one word out into many; an unquoted word that expands to
 * nothing vanishes entirely (array of zero). NULL on error. */
char **psh_expand_word(const char *raw, size_t *out_n);

/* Raw word → exactly one final string (no globbing) — for redirect
 * paths and assignment values, where fan-out makes no sense. */
char *psh_expand_word_single(const char *raw);

/* exec.c — run every statement; returns the last one's exit status */
int psh_execute(psh_stmt *stmts);

/* ---------------- jobs (jobs.c) ---------------- */

/* A job = one pipeline (or one background subshell) = one process
 * group. The struct is private to jobs.c. */
typedef struct psh_job psh_job;

/* True when this is an interactive shell that owns a terminal and
 * plays the process-group game. Off for scripts and pipes. */
extern bool psh_job_control;

void psh_jobs_init(bool interactive);
psh_job *psh_job_create(char *cmdline); /* takes ownership of cmdline */
void psh_job_add_pid(psh_job *j, pid_t pid);
size_t psh_job_npids(const psh_job *j);
pid_t psh_job_get_pgid(const psh_job *j);
void psh_job_set_pgid(psh_job *j, pid_t pgid);
void psh_job_discard(psh_job *j); /* nothing was spawned: forget it */
/* In the forked child, before exec: join the job's process group,
 * optionally take the terminal, reset job-control signals. */
void psh_job_child_setup(pid_t pgid, bool foreground);
int psh_job_foreground(psh_job *j, bool cont); /* wait; returns status */
void psh_job_background(psh_job *j);           /* announce [n] pgid */
void psh_jobs_reap(void);   /* silent non-blocking reap of children */
void psh_jobs_notify(void); /* reap + report Done/Stopped (per prompt) */
int psh_builtin_jobs(char **argv);
int psh_builtin_fg(char **argv);
int psh_builtin_bg(char **argv);
int psh_builtin_wait(char **argv);

/* complete.c — tab completion (commands first word, files elsewhere) */
void psh_completion_init(void);

/* builtins.c */
typedef int (*psh_builtin_fn)(char **argv);
psh_builtin_fn psh_find_builtin(const char *name);
void psh_list_builtins(void);
const char *psh_builtin_name(size_t i); /* NULL past the end */

/* pistachio.c 🫛 */
void psh_pistachio_hello(void);
const char *psh_pistachio_notfound(void);
int psh_builtin_crack(char **argv);

#endif /* PSH_H */
