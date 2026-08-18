/*
 * lexer.c — turn one line of input into a stream of tokens.
 *
 * Token kinds:  words,  |  ||  &&  ;  <  >  >>  2>
 *
 * Since milestone 3 the lexer no longer strips quotes: a word token
 * carries the RAW text, quotes and $ signs intact, and expand.c
 * finishes the job at execution time. The lexer's only quoting duties
 * are (a) letting quoted operator characters stay inside a word —
 * echo "a|b" is one word, no pipe — and (b) rejecting unterminated
 * quotes early.
 *
 * `2>` is recognized only at the start of a word, matching sh: in
 * `echo 2> f` the 2 binds to the redirect, but `echo a2> f` is the
 * word "a2" followed by a plain `>`.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psh.h"

static psh_token *tok_append(psh_token **head, psh_token **tail,
                             psh_token_type type, char *text)
{
    psh_token *t = malloc(sizeof *t);
    if (!t)
        return NULL;
    t->type = type;
    t->text = text; /* takes ownership */
    t->next = NULL;
    if (*tail)
        (*tail)->next = t;
    else
        *head = t;
    *tail = t;
    return t;
}

psh_token *psh_tokenize(const char *line, bool *err)
{
    *err = false;
    psh_token *head = NULL, *tail = NULL;
    /* One token can never be longer than the whole line. */
    char *buf = malloc(strlen(line) + 1);
    if (!buf)
        goto oom;

    const char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;

        int op = -1;
        if (*p == '&' && p[1] == '&') { op = TOK_AND; p += 2; }
        else if (*p == '|' && p[1] == '|') { op = TOK_OR; p += 2; }
        else if (*p == '|') { op = TOK_PIPE; p += 1; }
        else if (*p == ';') { op = TOK_SEMI; p += 1; }
        else if (*p == '<') { op = TOK_REDIR_IN; p += 1; }
        else if (*p == '>' && p[1] == '>') { op = TOK_REDIR_APPEND; p += 2; }
        else if (*p == '>') { op = TOK_REDIR_OUT; p += 1; }
        else if (*p == '2' && p[1] == '>') { op = TOK_REDIR_ERR; p += 2; }
        else if (*p == '&') {
            fprintf(stderr,
                    "psh: '&' (background jobs) arrives in a later "
                    "milestone\n");
            goto fail;
        }

        if (op >= 0) {
            if (!tok_append(&head, &tail, (psh_token_type)op, NULL))
                goto oom;
            continue;
        }

        /* A word: runs until unquoted whitespace or an operator char.
         * Quoted stretches are copied VERBATIM, quote marks included —
         * expansion and quote removal are expand.c's job, later. */
        size_t t = 0;
        while (*p && !isspace((unsigned char)*p) && !strchr("|&;<>", *p)) {
            if (*p == '\'' || *p == '"') {
                char quote = *p;
                buf[t++] = *p++;
                while (*p && *p != quote)
                    buf[t++] = *p++;
                if (!*p) {
                    fprintf(stderr,
                            "psh: syntax error: unterminated quote\n");
                    goto fail;
                }
                buf[t++] = *p++; /* keep the closing quote too */
            } else {
                buf[t++] = *p++;
            }
        }
        buf[t] = '\0';

        char *word = strdup(buf);
        if (!word || !tok_append(&head, &tail, TOK_WORD, word)) {
            free(word);
            goto oom;
        }
    }

    free(buf);
    return head;

oom:
    fprintf(stderr, "psh: out of memory\n");
fail:
    free(buf);
    psh_tokens_free(head);
    *err = true;
    return NULL;
}

void psh_tokens_free(psh_token *t)
{
    while (t) {
        psh_token *next = t->next;
        free(t->text);
        free(t);
        t = next;
    }
}
