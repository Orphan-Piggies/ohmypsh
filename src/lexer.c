/*
 * lexer.c — turn one line of input into a stream of tokens.
 *
 * In milestone 1 this file produced argv[] directly. Now that the
 * grammar has operators, the classic split appears: the lexer only
 * CLASSIFIES characters into tokens; parser.c gives them structure.
 *
 * Token kinds:  words,  |  ;  <  >  >>  2>
 *
 * Quoting rules are unchanged — '...' and "..." group text into one
 * word — with one important consequence: a QUOTED operator character
 * is literal.  echo "a|b"  is one word; no pipe is created.
 *
 * `2>` is recognized only at the start of a word, matching sh: in
 * `echo 2> f` the 2 binds to the redirect, but `echo a2> f` is the
 * word "a2" followed by a plain `>`. (`2>>` can join in a later
 * milestone if anyone ever misses it.)
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
        if (*p == '|') { op = TOK_PIPE; p += 1; }
        else if (*p == ';') { op = TOK_SEMI; p += 1; }
        else if (*p == '<') { op = TOK_REDIR_IN; p += 1; }
        else if (*p == '>' && p[1] == '>') { op = TOK_REDIR_APPEND; p += 2; }
        else if (*p == '>') { op = TOK_REDIR_OUT; p += 1; }
        else if (*p == '2' && p[1] == '>') { op = TOK_REDIR_ERR; p += 2; }

        if (op >= 0) {
            if (!tok_append(&head, &tail, (psh_token_type)op, NULL))
                goto oom;
            continue;
        }

        /* A word: runs until unquoted whitespace or an operator char. */
        size_t t = 0;
        while (*p && !isspace((unsigned char)*p) && !strchr("|;<>", *p)) {
            if (*p == '\'' || *p == '"') {
                char quote = *p++;
                while (*p && *p != quote)
                    buf[t++] = *p++;
                if (!*p) {
                    fprintf(stderr,
                            "psh: syntax error: unterminated quote\n");
                    goto fail;
                }
                p++; /* skip the closing quote */
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
