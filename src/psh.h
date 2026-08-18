/*
 * psh.h — shared declarations for the pistachio shell.
 *
 * The pipeline of the shell itself (fitting, really):
 *
 *   main.c      the REPL: prompt → read (readline) → run → repeat,
 *               plus scripts, -c, ~/.pshrc, multi-line accumulation
 *   lexer.c     raw input → token stream (words kept RAW, quotes and
 *               $(...) intact; newlines are tokens now)
 *   parser.c    recursive descent: tokens → statements — lists,
 *               if/while/for, function definitions
 *   expand.c    raw word → final strings: $VAR, $1..$9, $(...), ~,
 *               quote removal, glob — at execution time
 *   exec.c      runs the tree: pipelines, jobs, control flow, functions
 *   jobs.c      job control: process groups, tcsetpgrp, Ctrl-Z
 *   builtins.c  commands that must run inside the shell process
 *   complete.c  tab completion
 *   pistachio.c 🫛 flavor, banners, and easter pistachios
 *
 * Ordering that mirrors real shells: parse FIRST, expand at EXECUTION
 * time, remove quotes LAST — why `X=5; echo $X` prints 5.
 */
#ifndef PSH_H
#define PSH_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h> /* pid_t */

#define PSH_VERSION "0.7.0"

/*
 * The mascot glyph. Unicode has no pistachio emoji (🥜 is officially
 * PEANUTS, U+1F95C — an insult we do not accept), so: the pea pod.
 * Rebranding the whole shell is a one-line edit here.
 */
#define PSH_NUT "🫛"

/* Exit status of the last command — what $? expands to. main.c. */
extern int psh_last_status;

/* Set by the interactive SIGINT handler; loops check it and stop. */
extern volatile sig_atomic_t psh_interrupted;

/* Positional parameters: script args, or the current function's args
 * while one is running. expand.c reads these for $1..$9, $#, $0. */
extern char **psh_script_args;
extern size_t psh_script_argc;
extern const char *psh_arg0;

/* Run a complete string / a file through lex→parse→execute (main.c).
 * Incomplete input is an error here ("unexpected end of input"). */
int psh_run_string(const char *s);
int psh_run_file(const char *path);

/* ---------------- variables (vars.c) ---------------- */
/* Three tiers: function locals → shell vars → environment. Children
 * inherit only the environment; `export` promotes into it. */
const char *psh_var_get(const char *name); /* NULL if unset anywhere */
void psh_var_set(const char *name, const char *value);
void psh_var_export(const char *name);
void psh_var_unset(const char *name);
void psh_var_make_local(const char *name, const char *value);
bool psh_vars_in_function(void);
void psh_vars_push_scope(void); /* called around function bodies */
void psh_vars_pop_scope(void);

/* arith.c — the $(( ... )) evaluator */
long long psh_arith_eval(const char *expr, bool *err);

/* ---------------- tokens (lexer.c) ---------------- */

typedef enum {
    TOK_WORD,
    TOK_PIPE,         /* |  */
    TOK_AND,          /* && */
    TOK_OR,           /* || */
    TOK_SEMI,         /* ;  */
    TOK_DSEMI,        /* ;; — ends a case item */
    TOK_AMP,          /* &  */
    TOK_NEWLINE,      /* \n — a separator, except after | && || */
    TOK_LPAREN,       /* (  — only meaningful in name() definitions */
    TOK_RPAREN,       /* )  */
    TOK_REDIR_IN,     /* <  */
    TOK_REDIR_OUT,    /* >  */
    TOK_REDIR_APPEND, /* >> */
    TOK_REDIR_ERR,    /* 2> */
} psh_token_type;

typedef struct psh_token {
    psh_token_type type;
    char *text; /* TOK_WORD only: the RAW word, quotes/$() intact */
    struct psh_token *next;
} psh_token;

/* Three outcomes: tokens (NULL = blank input), *err with a message
 * printed, or *incomplete — the input is fine so far but unfinished
 * (open quote or $( ): the REPL should ask for another line. */
psh_token *psh_tokenize(const char *line, bool *err, bool *incomplete);
void psh_tokens_free(psh_token *t);

/* ---------------- syntax tree (parser.c) ---------------- */

/* One simple command: argv (raw words) + redirects. */
typedef struct psh_command {
    char **argv;
    size_t argc;
    char *in_path;  /* <  */
    char *out_path; /* > or >> */
    char *err_path; /* 2> */
    bool append;
    struct psh_command *next; /* next stage of the pipeline */
} psh_command;

typedef enum {
    PSH_CONN_END,
    PSH_CONN_AND, /* && */
    PSH_CONN_OR,  /* || */
} psh_conn;

typedef struct psh_andor {
    psh_command *pipeline;
    psh_conn conn;
    struct psh_andor *next;
} psh_andor;

/* A statement is a tagged union: a plain &&/|| list, or one of the
 * compound commands. Compounds reuse the shared cond/body slots. */
typedef enum {
    ST_LIST,    /* list */
    ST_IF,      /* cond, body (then), else_body — elif nests here */
    ST_WHILE,   /* cond, body */
    ST_FOR,     /* name, words[nwords] (raw), body */
    ST_CASE,    /* name (raw subject), case_items */
    ST_FUNCDEF, /* name, body */
} psh_stmt_kind;

typedef struct psh_case_item {
    char **patterns; /* raw; fnmatch'd against the subject at run time */
    size_t npatterns;
    struct psh_stmt *body; /* may be NULL: empty item */
    struct psh_case_item *next;
} psh_case_item;

typedef struct psh_stmt {
    psh_stmt_kind kind;
    bool background;             /* trailing & */
    psh_andor *list;             /* ST_LIST */
    struct psh_stmt *cond;       /* ST_IF, ST_WHILE */
    struct psh_stmt *body;       /* ST_IF then / loop body / function */
    struct psh_stmt *else_body;  /* ST_IF */
    char *name;                  /* ST_FOR var / ST_FUNCDEF / ST_CASE subject */
    char **words;                /* ST_FOR raw word list */
    size_t nwords;
    psh_case_item *case_items;   /* ST_CASE */
    struct psh_stmt *next;
} psh_stmt;

/* Same three outcomes as the lexer: a tree, an error (printed), or
 * *incomplete — e.g. an `if` still waiting for its `fi`. */
psh_stmt *psh_parse(psh_token *tokens, bool *err, bool *incomplete);
void psh_stmts_free(psh_stmt *s);

/* ---------------- expansion (expand.c) ---------------- */

char **psh_expand_word(const char *raw, size_t *out_n);
char *psh_expand_word_single(const char *raw); /* no glob, no split */

/* ---------------- execution (exec.c) ---------------- */

int psh_execute(psh_stmt *stmts);

/* True if a shell function with this name is defined (for hooks). */
bool psh_function_exists(const char *name);

/* Control flow raised by the break/continue/return builtins and
 * consumed by the loop / function executors. */
typedef enum {
    PSH_FLOW_NONE,
    PSH_FLOW_BREAK,
    PSH_FLOW_CONTINUE,
    PSH_FLOW_RETURN,
} psh_flow_t;
extern psh_flow_t psh_flow;

/* ---------------- jobs (jobs.c) ---------------- */

typedef struct psh_job psh_job;
extern bool psh_job_control;

void psh_jobs_init(bool interactive);
psh_job *psh_job_create(char *cmdline); /* takes ownership of cmdline */
void psh_job_add_pid(psh_job *j, pid_t pid);
size_t psh_job_npids(const psh_job *j);
pid_t psh_job_get_pgid(const psh_job *j);
void psh_job_set_pgid(psh_job *j, pid_t pgid);
void psh_job_discard(psh_job *j);
void psh_job_child_setup(pid_t pgid, bool foreground);
int psh_job_foreground(psh_job *j, bool cont);
void psh_job_background(psh_job *j);
void psh_jobs_reap(void);
void psh_jobs_notify(void);
int psh_builtin_jobs(char **argv);
int psh_builtin_fg(char **argv);
int psh_builtin_bg(char **argv);
int psh_builtin_wait(char **argv);

/* complete.c */
void psh_completion_init(void);

/* builtins.c */
typedef int (*psh_builtin_fn)(char **argv);
psh_builtin_fn psh_find_builtin(const char *name);
void psh_list_builtins(void);
const char *psh_builtin_name(size_t i);

/* pistachio.c 🫛 */
void psh_pistachio_hello(void);
const char *psh_pistachio_notfound(void);
int psh_builtin_crack(char **argv);

#endif /* PSH_H */
