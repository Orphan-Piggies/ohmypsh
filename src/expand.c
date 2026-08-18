/*
 * expand.c — raw word → final string(s), at execution time.
 *
 * Order of operations, sh's canonical sequence:
 *
 *   1. tilde:      leading unquoted ~  →  $HOME
 *   2. variables:  $NAME ${NAME} $? $$ $0..$9 $#   (inside "..." too,
 *                  never inside '...')
 *   3. cmd subst:  $( commands ) — run them, capture stdout, strip
 *                  trailing newlines
 *   4. splitting:  ONLY the output of an unquoted $(...) splits, and
 *                  only on NEWLINES (fish's rule). `for f in $(ls)`
 *                  iterates lines; a $VAR still NEVER splits — the
 *                  founding psh rule survives command substitution.
 *   5. globbing:   * ? [  — suppressed if any part was quoted
 *   6. quote removal, last of all
 *
 * An unquoted word that expands to nothing vanishes; an explicit ""
 * stays as a real empty argument.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

typedef struct {
    bool had_quote;  /* any quoted stretch: suppresses glob and split */
    bool had_cmdsub; /* a $( ) ran: its newlines may split the word */
} xflags;

/*
 * $( commands ): fork a subshell whose stdout is a pipe, run the
 * commands through the ordinary lex→parse→execute path, read
 * everything, strip trailing newlines. The child gets default
 * signals and no job control — it is plumbing, not a terminal user.
 */
static bool capture_cmdsub(const char *cmd, sbuf *b)
{
    int fds[2];
    if (pipe(fds) < 0)
        return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        psh_job_control = false;
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        _exit(psh_run_string(cmd) & 0xff);
    }
    close(fds[1]);
    size_t start = b->len;
    char tmp[512];
    ssize_t n;
    while ((n = read(fds[0], tmp, sizeof tmp)) > 0)
        for (ssize_t i = 0; i < n; i++)
            sb_putc(b, tmp[i]);
    close(fds[0]);
    int ws;
    waitpid(pid, &ws, 0); /* this pid precisely — never a job's */
    while (b->len > start && b->s[b->len - 1] == '\n')
        b->s[--b->len] = '\0';
    return true;
}

/* Find the ')' matching the '(' at p (which points AFTER "$("),
 * skipping quoted stretches. The lexer already guaranteed balance. */
static const char *cmdsub_end(const char *p)
{
    int depth = 1;
    while (*p) {
        if (*p == '\'' || *p == '"') {
            char q = *p++;
            while (*p && *p != q)
                p++;
            if (*p)
                p++;
            continue;
        }
        if (*p == '(')
            depth++;
        if (*p == ')' && --depth == 0)
            return p;
        p++;
    }
    return p;
}

/*
 * Expand one $-form. p points just AFTER the '$'. Appends the value
 * and returns where scanning should continue. A '$' that starts
 * nothing recognizable stays a literal '$'.
 */
static const char *expand_dollar(const char *p, sbuf *b, xflags *fl)
{
    char num[24];

    if (*p == '(') { /* command substitution */
        const char *end = cmdsub_end(p + 1);
        char *inner = strndup(p + 1, (size_t)(end - (p + 1)));
        if (inner) {
            capture_cmdsub(inner, b);
            free(inner);
            fl->had_cmdsub = true;
        }
        return *end ? end + 1 : end;
    }
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
    if (*p == '#') {
        snprintf(num, sizeof num, "%zu", psh_script_argc);
        sb_puts(b, num);
        return p + 1;
    }
    if (isdigit((unsigned char)*p)) { /* positional: $0..$9 */
        int idx = *p - '0';
        if (idx == 0)
            sb_puts(b, psh_arg0 ? psh_arg0 : "psh");
        else if ((size_t)idx <= psh_script_argc)
            sb_puts(b, psh_script_args[idx - 1]);
        return p + 1;
    }

    const char *start = p;
    const char *end;
    bool braced = (*p == '{');
    if (braced) {
        start = p + 1;
        end = strchr(start, '}');
        if (!end || end == start) {
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
    return braced ? end + 1 : end;
}

/* Steps 1–3 and 6: everything except globbing and splitting. */
static char *expand_core(const char *raw, xflags *fl)
{
    sbuf b = { 0 };
    fl->had_quote = false;
    fl->had_cmdsub = false;
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
        if (*p == '\'') { /* verbatim: nothing expands in '...' */
            fl->had_quote = true;
            p++;
            while (*p && *p != '\'')
                sb_putc(&b, *p++);
            if (*p)
                p++;
        } else if (*p == '"') { /* $ expands, the rest is verbatim */
            fl->had_quote = true;
            p++;
            while (*p && *p != '"') {
                if (*p == '$')
                    p = expand_dollar(p + 1, &b, fl);
                else
                    sb_putc(&b, *p++);
            }
            if (*p)
                p++;
        } else if (*p == '$') {
            p = expand_dollar(p + 1, &b, fl);
        } else {
            sb_putc(&b, *p++);
        }
    }
    return b.s;
}

char *psh_expand_word_single(const char *raw)
{
    xflags fl;
    return expand_core(raw, &fl);
}

/* Append `word` to the vector, globbing it if eligible. A pattern
 * that matches nothing stays literal (sh's default). */
static bool add_globbed(char ***v, size_t *n, size_t *cap,
                        const char *word, bool allow_glob)
{
    char **matches = NULL;
    size_t nmatches = 0;
    glob_t g;
    bool used_glob = false;

    if (allow_glob && strpbrk(word, "*?[") &&
        glob(word, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        matches = g.gl_pathv;
        nmatches = g.gl_pathc;
        used_glob = true;
    }

    size_t add = used_glob ? nmatches : 1;
    if (*n + add + 1 > *cap) {
        size_t newcap = (*n + add + 1) * 2;
        char **grown = realloc(*v, newcap * sizeof **v);
        if (!grown) {
            if (used_glob)
                globfree(&g);
            return false;
        }
        *v = grown;
        *cap = newcap;
    }
    if (used_glob) {
        for (size_t i = 0; i < nmatches; i++)
            (*v)[(*n)++] = strdup(matches[i]);
        globfree(&g);
    } else {
        (*v)[(*n)++] = strdup(word);
    }
    return true;
}

char **psh_expand_word(const char *raw, size_t *out_n)
{
    xflags fl;
    char *s = expand_core(raw, &fl);
    if (!s)
        return NULL;

    size_t cap = 4, n = 0;
    char **v = malloc(cap * sizeof *v);
    if (!v) {
        free(s);
        return NULL;
    }

    if (s[0] == '\0' && !fl.had_quote) {
        /* Unquoted and expanded to nothing → the word vanishes. */
        free(s);
        v[0] = NULL;
        *out_n = 0;
        return v;
    }

    if (fl.had_cmdsub && !fl.had_quote && strchr(s, '\n')) {
        /* Unquoted $( ) output: split on newlines, skip empties,
         * glob each resulting word. */
        char *sp = NULL;
        for (char *piece = strtok_r(s, "\n", &sp); piece;
             piece = strtok_r(NULL, "\n", &sp))
            if (*piece)
                add_globbed(&v, &n, &cap, piece, true);
        free(s);
    } else {
        add_globbed(&v, &n, &cap, s, !fl.had_quote);
        free(s);
    }
    v[n] = NULL;
    *out_n = n;
    return v;
}
