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
#include <fnmatch.h>
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

static char *expand_core(const char *raw, xflags *fl);

/* ---------------- H9.1: ${...} parameter operators ---------------- */

/* ${var#pat} ${var##pat} ${var%pat} ${var%%pat}: strip the
 * shortest/longest matching prefix/suffix. Patterns are fnmatch
 * globs, as in sh. O(n²) trial matching — a shell word, not a
 * genome. */
static void strip_affix(const char *val, const char *pat, bool suffix,
                        bool longest, sbuf *b)
{
    size_t n = strlen(val);
    long hit = -1;
    if (!suffix) {
        for (size_t i = 0; i <= n; i++) {
            char *pre = strndup(val, i);
            bool m = pre && fnmatch(pat, pre, 0) == 0;
            free(pre);
            if (m) {
                hit = (long)i;
                if (!longest)
                    break;
            }
        }
        sb_puts(b, hit >= 0 ? val + hit : val);
        return;
    }
    /* suffix: shortest = latest start index, longest = earliest */
    for (size_t i = 0; i <= n; i++) {
        size_t idx = longest ? i : n - i;
        if (fnmatch(pat, val + idx, 0) == 0) {
            hit = (long)idx;
            break;
        }
    }
    if (hit < 0) {
        sb_puts(b, val);
        return;
    }
    for (long i = 0; i < hit; i++)
        sb_putc(b, val[i]);
}

/* ${var/old/new} (first) and ${var//old/new} (all). psh flavor,
 * documented: old is a LITERAL string, not a pattern — predictable
 * beats clever in the one operator people reach for daily. */
static void replace_lit(const char *val, const char *old,
                        const char *neu, bool all, sbuf *b)
{
    if (!*old) {
        sb_puts(b, val);
        return;
    }
    const char *p = val;
    const char *hit;
    while ((hit = strstr(p, old))) {
        while (p < hit)
            sb_putc(b, *p++);
        sb_puts(b, neu);
        p += strlen(old);
        if (!all)
            break;
    }
    sb_puts(b, p);
}

/* The inside of a ${ ... }: name, ${#name}, multi-digit positionals,
 * braced specials, and the operators. Unknown forms COMPLAIN — the
 * Google audit caught ${x#a} silently expanding to empty (it read as
 * a variable named "x#a"); silence was the bug. */
static void expand_braced(const char *content, sbuf *b)
{
    char num[32];

    /* braced specials, same meanings as their bare forms */
    if (content[1] == '\0') {
        switch (content[0]) {
        case '?':
            snprintf(num, sizeof num, "%d", psh_last_status);
            sb_puts(b, num);
            return;
        case '$':
            snprintf(num, sizeof num, "%d", (int)getpid());
            sb_puts(b, num);
            return;
        case '#':
            snprintf(num, sizeof num, "%zu", psh_script_argc);
            sb_puts(b, num);
            return;
        case '@':
            for (size_t i = 0; i < psh_script_argc; i++) {
                sb_puts(b, psh_script_args[i]);
                if (i + 1 < psh_script_argc)
                    sb_putc(b, ' ');
            }
            return;
        }
    }

    /* ${10}: multi-digit positionals, at last */
    bool alldigits = true;
    for (const char *q = content; *q; q++)
        if (!isdigit((unsigned char)*q))
            alldigits = false;
    if (alldigits && *content) {
        long idx = strtol(content, NULL, 10);
        if (idx == 0)
            sb_puts(b, psh_arg0 ? psh_arg0 : "psh");
        else if ((size_t)idx <= psh_script_argc)
            sb_puts(b, psh_script_args[idx - 1]);
        return;
    }

    /* ${#name}: length in bytes (0 when unset) */
    if (content[0] == '#') {
        const char *nm = content + 1;
        bool ok = isalpha((unsigned char)nm[0]) || nm[0] == '_';
        for (const char *q = nm + 1; ok && *q; q++)
            ok = isalnum((unsigned char)*q) || *q == '_';
        if (ok) {
            const char *val = psh_var_get(nm);
            snprintf(num, sizeof num, "%zu", val ? strlen(val) : 0);
            sb_puts(b, num);
            return;
        }
        fprintf(stderr, "psh: ${%s}: bad substitution\n", content);
        return;
    }

    /* a NAME, then maybe an operator */
    size_t i = 0;
    if (isalpha((unsigned char)content[0]) || content[0] == '_') {
        i = 1;
        while (isalnum((unsigned char)content[i]) || content[i] == '_')
            i++;
    }
    if (i == 0) {
        fprintf(stderr, "psh: ${%s}: bad substitution\n", content);
        return;
    }
    char name[256];
    size_t nn = i < sizeof name ? i : sizeof name - 1;
    memcpy(name, content, nn);
    name[nn] = '\0';
    const char *rest = content + i;
    const char *val = psh_var_get(name);

    if (!*rest) { /* plain ${NAME} */
        if (val)
            sb_puts(b, val);
        return;
    }
    if (!val)
        val = ""; /* operators on unset act on the empty string */

    if (*rest == '#' || *rest == '%') {
        bool suffix = (*rest == '%');
        bool longest = rest[1] == *rest;
        const char *rawpat = rest + (longest ? 2 : 1);
        xflags fl;
        char *pat = expand_core(rawpat, &fl); /* ${x%$ext} works */
        if (pat) {
            strip_affix(val, pat, suffix, longest, b);
            free(pat);
        }
        return;
    }
    if (*rest == '/') {
        bool all = rest[1] == '/';
        const char *from = rest + (all ? 2 : 1);
        const char *sep = strchr(from, '/');
        char *rawold = sep ? strndup(from, (size_t)(sep - from))
                           : strdup(from);
        const char *rawnew = sep ? sep + 1 : "";
        xflags fl;
        char *old = rawold ? expand_core(rawold, &fl) : NULL;
        char *neu = expand_core(rawnew, &fl);
        if (old && neu)
            replace_lit(val, old, neu, all, b);
        free(rawold);
        free(old);
        free(neu);
        return;
    }
    fprintf(stderr, "psh: ${%s}: bad substitution\n", content);
}

/*
 * Expand one $-form. p points just AFTER the '$'. Appends the value
 * and returns where scanning should continue. A '$' that starts
 * nothing recognizable stays a literal '$'.
 */
static const char *expand_dollar(const char *p, sbuf *b, xflags *fl)
{
    char num[32];

    if (*p == '(' && p[1] == '(') { /* $(( arithmetic )) */
        const char *end = cmdsub_end(p + 2); /* the first closing ')' */
        if (end[0] == ')' && end[1] == ')') {
            char *inner = strndup(p + 2, (size_t)(end - (p + 2)));
            if (inner) {
                bool aerr = false;
                long long v = psh_arith_eval(inner, &aerr);
                if (aerr) {
                    fprintf(stderr, "psh: $(( %s )): bad expression\n",
                            inner);
                } else {
                    snprintf(num, sizeof num, "%lld", v);
                    sb_puts(b, num);
                }
                free(inner);
            }
            return end + 2;
        }
        /* only one closing paren: fall through — it's a cmdsub */
    }
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
    if (*p == '@') { /* embedded $@: args joined by spaces. A word
                        that is EXACTLY $@ never reaches here — it
                        splats into a real list in psh_expand_word. */
        for (size_t i = 0; i < psh_script_argc; i++) {
            sb_puts(b, psh_script_args[i]);
            if (i + 1 < psh_script_argc)
                sb_putc(b, ' ');
        }
        return p + 1;
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

    if (*p == '{') { /* ${...}: names, specials, and the H9.1 ops */
        const char *start = p + 1;
        const char *end = strchr(start, '}');
        if (!end || end == start) {
            sb_putc(b, '$');
            return p;
        }
        char *content = strndup(start, (size_t)(end - start));
        if (content) {
            expand_braced(content, b);
            free(content);
        }
        return end + 1;
    }

    if (!isalpha((unsigned char)*p) && *p != '_') {
        sb_putc(b, '$');
        return p;
    }
    const char *end = p;
    while (isalnum((unsigned char)*end) || *end == '_')
        end++;

    char name[256];
    size_t n = (size_t)(end - p);
    if (n >= sizeof name)
        n = sizeof name - 1;
    memcpy(name, p, n);
    name[n] = '\0';

    const char *val = psh_var_get(name); /* locals → shell → environ */
    if (val)
        sb_puts(b, val);
    return end;
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
        const char *home = psh_var_get("HOME");
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
    /* The splat: a word that IS $@ (or "$@") becomes one word per
     * argument — a real list, nothing re-split, spaces intact. This
     * is what makes `wrapper() { real-tool $@; }` simply correct. */
    if (strcmp(raw, "$@") == 0 || strcmp(raw, "\"$@\"") == 0) {
        char **v = calloc(psh_script_argc + 1, sizeof *v);
        if (!v)
            return NULL;
        for (size_t i = 0; i < psh_script_argc; i++)
            v[i] = strdup(psh_script_args[i]);
        *out_n = psh_script_argc;
        return v;
    }

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
