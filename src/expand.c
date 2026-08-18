/*
 * expand.c — raw word → final string(s), at execution time.
 *
 * A raw word arrives with its quotes still inside ( echo "$HOME"/'*'
 * is ONE raw word: "$HOME"/'*' ). This module makes the final strings
 * a command actually receives, applying — in sh's canonical order:
 *
 *   1. tilde:     leading unquoted ~  →  $HOME
 *   2. variables: $NAME  ${NAME}  $?  $$   (inside "..." too;
 *                 never inside '...')
 *   3. globbing:  * ? [  fan a word out into matching paths —
 *                 but only if NO part of the word was quoted
 *   4. quote removal: the quote marks themselves disappear last
 *
 * The founding psh rule lives here, by OMISSION: there is no word
 * splitting step. In sh, `F="two words"; rm $F` re-splits the value
 * and deletes two files. In psh an expansion is always exactly one
 * word (globbing aside) — `rm $F` removes "two words", quoted or not.
 *
 * One nuance kept from sh because it is genuinely useful: an unquoted
 * word that expands to nothing vanishes ( `ls $FLAGS` with FLAGS unset
 * runs plain `ls` ), while an explicitly quoted empty string ( "" )
 * stays as a real empty argument.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psh.h"

/* A tiny growable string. */
typedef struct {
    char *s;
    size_t len, cap;
} sbuf;

static bool sb_reserve(sbuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 32;
    while (cap < b->len + extra + 1)
        cap *= 2;
    char *grown = realloc(b->s, cap);
    if (!grown)
        return false;
    b->s = grown;
    b->cap = cap;
    return true;
}

static bool sb_putc(sbuf *b, char ch)
{
    if (!sb_reserve(b, 1))
        return false;
    b->s[b->len++] = ch;
    b->s[b->len] = '\0';
    return true;
}

static bool sb_puts(sbuf *b, const char *s)
{
    size_t n = strlen(s);
    if (!sb_reserve(b, n))
        return false;
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
    return true;
}

/*
 * Expand one $-form. p points just AFTER the '$'. Appends the value
 * and returns where scanning should continue. A '$' that starts
 * nothing recognizable stays a literal '$' (so `echo cost: 5$` works).
 */
static const char *expand_dollar(const char *p, sbuf *b)
{
    char num[16];

    if (*p == '?') {
        snprintf(num, sizeof num, "%d", psh_last_status);
        sb_puts(b, num);
        return p + 1;
    }
    if (*p == '$') {
        snprintf(num, sizeof num, "%d", (int)getpid());
        sb_puts(b, num);
        return p + 1;
    }

    const char *start = p;
    const char *end;
    bool braced = (*p == '{');
    if (braced) {
        start = p + 1;
        end = strchr(start, '}');
        if (!end || end == start) { /* ${ with no close: literal $ */
            sb_putc(b, '$');
            return p;
        }
    } else {
        if (!isalpha((unsigned char)*p) && *p != '_') {
            sb_putc(b, '$');
            return p;
        }
        end = p;
        while (isalnum((unsigned char)*end) || *end == '_')
            end++;
    }

    char name[256];
    size_t n = (size_t)(end - start);
    if (n >= sizeof name)
        n = sizeof name - 1;
    memcpy(name, start, n);
    name[n] = '\0';

    const char *val = getenv(name);
    if (val)
        sb_puts(b, val);
    /* unset → empty, silently; `set -u` strictness can come later */
    return braced ? end + 1 : end;
}

/* Steps 1, 2 and 4: everything except globbing. */
static char *expand_core(const char *raw, bool *had_quote)
{
    sbuf b = { 0 };
    *had_quote = false;
    if (!sb_reserve(&b, strlen(raw)))
        return NULL;
    /* A word can legally expand to NOTHING ($UNSET alone) — no sb_*
     * call would ever run, so terminate the empty string up front or
     * we hand out uninitialized malloc bytes as an argument. */
    b.s[0] = '\0';

    const char *p = raw;
    if (p[0] == '~' && (p[1] == '/' || p[1] == '\0')) {
        const char *home = getenv("HOME");
        sb_puts(&b, home ? home : "~");
        p++;
    }

    while (*p) {
        if (*p == '\'') { /* verbatim: no $ inside single quotes */
            *had_quote = true;
            p++;
            while (*p && *p != '\'')
                sb_putc(&b, *p++);
            if (*p)
                p++;
        } else if (*p == '"') { /* $ expands, everything else verbatim */
            *had_quote = true;
            p++;
            while (*p && *p != '"') {
                if (*p == '$')
                    p = expand_dollar(p + 1, &b);
                else
                    sb_putc(&b, *p++);
            }
            if (*p)
                p++;
        } else if (*p == '$') {
            p = expand_dollar(p + 1, &b);
        } else {
            sb_putc(&b, *p++);
        }
    }
    return b.s; /* always NUL-terminated by sb_* */
}

char *psh_expand_word_single(const char *raw)
{
    bool had_quote;
    return expand_core(raw, &had_quote);
}

char **psh_expand_word(const char *raw, size_t *out_n)
{
    bool had_quote;
    char *s = expand_core(raw, &had_quote);
    if (!s)
        return NULL;

    /* Unquoted and expanded to nothing → the word simply vanishes. */
    if (s[0] == '\0' && !had_quote) {
        free(s);
        *out_n = 0;
        return calloc(1, sizeof(char *));
    }

    /* Step 3: globbing — suppressed if ANY part was quoted, so
     * `echo "*.c"` prints the literal pattern. A pattern that matches
     * nothing also stays literal (sh's default; fish would error). */
    if (!had_quote && strpbrk(s, "*?[")) {
        glob_t g;
        if (glob(s, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
            char **v = calloc(g.gl_pathc + 1, sizeof *v);
            if (!v) {
                globfree(&g);
                free(s);
                return NULL;
            }
            for (size_t i = 0; i < g.gl_pathc; i++)
                v[i] = strdup(g.gl_pathv[i]);
            *out_n = g.gl_pathc;
            globfree(&g);
            free(s);
            return v;
        }
    }

    char **v = calloc(2, sizeof *v);
    if (!v) {
        free(s);
        return NULL;
    }
    v[0] = s;
    *out_n = 1;
    return v;
}
