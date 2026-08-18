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

#define PSH_VERSION "0.3.0"

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

/* One statement = one &&/|| list. Statements are separated by ';'. */
typedef struct psh_stmt {
    psh_andor *list;
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

/* builtins.c */
typedef int (*psh_builtin_fn)(char **argv);
psh_builtin_fn psh_find_builtin(const char *name);
void psh_list_builtins(void);

/* pistachio.c 🫛 */
void psh_pistachio_hello(void);
const char *psh_pistachio_notfound(void);
int psh_builtin_crack(char **argv);

#endif /* PSH_H */
