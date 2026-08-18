/*
 * lexer.c — turn one line of input into an argv[] vector.
 *
 * Milestone-1 grammar, deliberately tiny:
 *
 *   - words are separated by unquoted whitespace
 *   - 'single quotes' and "double quotes" group text (including spaces)
 *     into one word; the quotes themselves are removed
 *   - quotes can sit inside a word:  ab"c d"e  →  one word: abc de
 *
 * Design decision (the "familiar core" philosophy): quoting exists only
 * HERE, at parse time. When variables arrive in a later milestone their
 * values will NOT be re-split on spaces — that single choice kills the
 * most famous family of sh bugs (`rm $file` deleting two files).
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "psh.h"

psh_command *psh_parse_line(const char *line)
{
    size_t cap = 8;
    size_t argc = 0;
    char **argv = malloc(cap * sizeof *argv);
    /*
     * One reusable token buffer. A single token can never be longer
     * than the whole line, so strlen(line)+1 bytes is always enough —
     * no reallocation logic needed inside the hot loop.
     */
    char *tok = malloc(strlen(line) + 1);
    if (!argv || !tok)
        goto oom;

    const char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;

        size_t t = 0;
        while (*p && !isspace((unsigned char)*p)) {
            if (*p == '\'' || *p == '"') {
                char quote = *p++;
                while (*p && *p != quote)
                    tok[t++] = *p++;
                if (!*p)
                    goto syntax_error; /* line ended inside quotes */
                p++; /* skip the closing quote */
            } else {
                tok[t++] = *p++;
            }
        }
        tok[t] = '\0';

        if (argc + 2 > cap) { /* +2: this token and the final NULL */
            cap *= 2;
            char **grown = realloc(argv, cap * sizeof *argv);
            if (!grown)
                goto oom;
            argv = grown;
        }
        argv[argc] = strdup(tok);
        if (!argv[argc])
            goto oom;
        argc++;
    }

    free(tok);
    argv[argc] = NULL;

    psh_command *cmd = malloc(sizeof *cmd);
    if (!cmd)
        goto oom_cmd;
    cmd->argv = argv;
    cmd->argc = argc;
    return cmd;

syntax_error:
    free(tok);
    for (size_t i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
    return NULL;

oom:
    free(tok);
oom_cmd:
    for (size_t i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
    return NULL;
}

void psh_command_free(psh_command *cmd)
{
    if (!cmd)
        return;
    for (size_t i = 0; i < cmd->argc; i++)
        free(cmd->argv[i]);
    free(cmd->argv);
    free(cmd);
}
