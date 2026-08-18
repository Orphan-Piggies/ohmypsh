/*
 * lexer.c — turn input text into a stream of tokens.
 *
 * Token kinds:  words,  |  ||  &&  &  ;  newline  ( )  <  >  >>  2>
 *
 * Words are kept RAW — quotes and $(...) stay inside the token text;
 * expand.c finishes them at execution time. The lexer's quoting
 * duties are: quoted operator characters stay in the word (echo "a|b"
 * is one word), a $( ... ) is one indivisible chunk of its word even
 * when it contains spaces, pipes or newlines, and an UNFINISHED quote
 * or $( is reported as *incomplete — not an error — so the REPL can
 * ask for another line. That one bit is what makes multi-line input
 * work.
 *
 * Newlines are tokens now (not just whitespace): inside `if`/`for`
 * they separate commands the way ';' does, and after | && || the
 * parser skips them so commands can continue across lines.
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

/*
 * Copy "$(" ... ")" verbatim into buf, tracking paren depth and
 * skipping over quoted stretches (a ')' inside quotes doesn't count).
 * Returns the position after the closing ')', or NULL if the input
 * ended first — the "incomplete" case.
 */
static const char *scan_cmdsub(const char *p, char *buf, size_t *t)
{
    buf[(*t)++] = *p++; /* '$' */
    buf[(*t)++] = *p++; /* '(' */
    int depth = 1;
    while (*p) {
        char ch = *p;
        if (ch == '\'' || ch == '"') {
            buf[(*t)++] = *p++;
            while (*p && *p != ch)
                buf[(*t)++] = *p++;
            if (!*p)
                return NULL;
            buf[(*t)++] = *p++;
            continue;
        }
        if (ch == '(')
            depth++;
        if (ch == ')') {
            depth--;
            if (depth == 0) {
                buf[(*t)++] = *p++;
                return p;
            }
        }
        buf[(*t)++] = *p++;
    }
    return NULL;
}

psh_token *psh_tokenize(const char *line, bool *err, bool *incomplete)
{
    *err = false;
    *incomplete = false;
    psh_token *head = NULL, *tail = NULL;
    /* One token can never be longer than the whole input. */
    char *buf = malloc(strlen(line) + 1);
    if (!buf)
        goto oom;

    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        if (!*p)
            break;

        if (*p == '\n') {
            if (!tok_append(&head, &tail, TOK_NEWLINE, NULL))
                goto oom;
            p++;
            continue;
        }

        /* '#' at the start of a word comments out the rest of the
         * LINE (not the buffer — later lines still count). */
        if (*p == '#') {
            while (*p && *p != '\n')
                p++;
            continue;
        }

        int op = -1;
        if (*p == '&' && p[1] == '&') { op = TOK_AND; p += 2; }
        else if (*p == '|' && p[1] == '|') { op = TOK_OR; p += 2; }
        else if (*p == '|') { op = TOK_PIPE; p += 1; }
        else if (*p == '&') { op = TOK_AMP; p += 1; }
        else if (*p == ';' && p[1] == ';') { op = TOK_DSEMI; p += 2; }
        else if (*p == ';') { op = TOK_SEMI; p += 1; }
        else if (*p == '(') { op = TOK_LPAREN; p += 1; }
        else if (*p == ')') { op = TOK_RPAREN; p += 1; }
        else if (*p == '<') { op = TOK_REDIR_IN; p += 1; }
        else if (*p == '>' && p[1] == '>') { op = TOK_REDIR_APPEND; p += 2; }
        else if (*p == '>') { op = TOK_REDIR_OUT; p += 1; }
        else if (*p == '2' && p[1] == '>') { op = TOK_REDIR_ERR; p += 2; }

        if (op >= 0) {
            if (!tok_append(&head, &tail, (psh_token_type)op, NULL))
                goto oom;
            continue;
        }

        /* A word: runs until unquoted whitespace or an operator. */
        size_t t = 0;
        while (*p && *p != '\n' && *p != ' ' && *p != '\t' &&
               *p != '\r' && !strchr("|&;<>()", *p)) {
            if (*p == '$' && p[1] == '(') {
                const char *np = scan_cmdsub(p, buf, &t);
                if (!np)
                    goto need_more;
                p = np;
            } else if (*p == '\'' || *p == '"') {
                char quote = *p;
                buf[t++] = *p++;
                while (*p && *p != quote) {
                    if (quote == '"' && *p == '$' && p[1] == '(') {
                        const char *np = scan_cmdsub(p, buf, &t);
                        if (!np)
                            goto need_more;
                        p = np;
                    } else {
                        buf[t++] = *p++;
                    }
                }
                if (!*p)
                    goto need_more; /* multi-line string: keep typing */
                buf[t++] = *p++;    /* keep the closing quote */
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
    free(buf);
    psh_tokens_free(head);
    *err = true;
    return NULL;

need_more:
    free(buf);
    psh_tokens_free(head);
    *incomplete = true;
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
