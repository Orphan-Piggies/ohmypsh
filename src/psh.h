/*
 * psh.h — shared declarations for the pistachio shell.
 *
 * The pipeline of the shell itself (fitting, really):
 *
 *   main.c      the REPL: prompt → read → run → repeat
 *   lexer.c     raw line → token stream (words and operators)
 *   parser.c    token stream → statements → pipelines → commands
 *   exec.c      runs a syntax tree: builtins, fork/exec, pipes, redirects
 *   builtins.c  commands that must run inside the shell process itself
 *   pistachio.c 🫛 flavor, banners, and easter pistachios
 */
#ifndef PSH_H
#define PSH_H

#include <stdbool.h>
#include <stddef.h>

#define PSH_VERSION "0.2.0"

/*
 * The mascot glyph, used in the prompt and banners. Unicode has no
 * pistachio emoji (🥜 is officially PEANUTS, U+1F95C — an insult we do
 * not accept), so we use the pea pod: green, lives in a shell, close
 * enough. Rebranding the whole shell is a one-line edit here.
 */
#define PSH_NUT "🫛"

/* ---------------- tokens (lexer.c) ---------------- */

typedef enum {
    TOK_WORD,
    TOK_PIPE,         /* |  */
    TOK_SEMI,         /* ;  */
    TOK_REDIR_IN,     /* <  */
    TOK_REDIR_OUT,    /* >  */
    TOK_REDIR_APPEND, /* >> */
    TOK_REDIR_ERR,    /* 2> */
} psh_token_type;

typedef struct psh_token {
    psh_token_type type;
    char *text; /* TOK_WORD only; NULL for operators */
    struct psh_token *next;
} psh_token;

/* Returns the token list (NULL for a blank line). On a syntax error,
 * prints a message, sets *err, and returns NULL. */
psh_token *psh_tokenize(const char *line, bool *err);
void psh_tokens_free(psh_token *t);

/* ---------------- syntax tree (parser.c) ---------------- */

/* One command: what to run, plus where its three standard streams go.
 * Repeated redirects of the same kind follow sh: the last one wins. */
typedef struct psh_command {
    char **argv;  /* NULL-terminated, as execvp requires */
    size_t argc;
    char *in_path;  /* <  */
    char *out_path; /* > or >> */
    char *err_path; /* 2> */
    bool append;    /* out_path came from >> */
    struct psh_command *next; /* next stage of the pipeline */
} psh_command;

/* One statement = one pipeline. Statements are separated by ';'. */
typedef struct psh_stmt {
    psh_command *pipeline;
    struct psh_stmt *next;
} psh_stmt;

psh_stmt *psh_parse(psh_token *tokens, bool *err);
void psh_stmts_free(psh_stmt *s);

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
