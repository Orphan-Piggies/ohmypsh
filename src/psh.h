/*
 * psh.h — shared declarations for the pistachio shell.
 *
 * The shell is split into four small modules:
 *
 *   main.c      the REPL: print prompt, read line, dispatch, repeat
 *   lexer.c     turns a raw input line into an argv[] vector
 *   exec.c      runs a parsed command (builtin, or fork + exec)
 *   builtins.c  commands that must run inside the shell process itself
 *   pistachio.c 🥜 flavor, banners, and easter pistachios
 */
#ifndef PSH_H
#define PSH_H

#include <stddef.h>

#define PSH_VERSION "0.1.0"

/*
 * The mascot glyph, used in the prompt and banners. Unicode has no
 * pistachio emoji (🥜 is officially PEANUTS, U+1F95C — an insult we do
 * not accept), so we use the pea pod: green, lives in a shell, close
 * enough. Rebranding the whole shell is a one-line edit here.
 */
#define PSH_NUT "🫛"

/*
 * A parsed command line. For milestone 1 this is a single command:
 * argv is NULL-terminated (execvp requires that), argc is the count
 * excluding the NULL. Pipelines will turn this into a list later.
 */
typedef struct {
    char **argv;
    size_t argc;
} psh_command;

/* lexer.c */
psh_command *psh_parse_line(const char *line); /* NULL on syntax error */
void psh_command_free(psh_command *cmd);

/* exec.c — returns the command's exit status (0 = success) */
int psh_execute(psh_command *cmd);

/* builtins.c */
typedef int (*psh_builtin_fn)(char **argv);
psh_builtin_fn psh_find_builtin(const char *name);
void psh_list_builtins(void);

/* pistachio.c 🥜 */
void psh_pistachio_hello(void);              /* interactive startup banner */
const char *psh_pistachio_notfound(void);    /* flavor for command-not-found */
int psh_builtin_crack(char **argv);          /* try it and see */

#endif /* PSH_H */
