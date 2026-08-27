/*
 * salt — the pistachio armory's answer to cat and bat. 🫛🧂
 *
 * You salt a pistachio; salt colors a file. The founding rule, and
 * the reason it exists at all:
 *
 *   SALT NEVER CHANGES THE BYTES. It only colors them.
 *
 * Which buys, for free, everything bat gets wrong for daily use:
 *
 *   - Copy-safe: select any region, paste it anywhere — you get the
 *     exact source, because the output IS the source. No gutters,
 *     no line numbers (unless you ask with -n), no box drawing.
 *   - Pipe-native: a line-at-a-time state machine, so
 *     `tail -f log | salt -l yaml` highlights live, and when stdout
 *     is not a terminal salt emits pure bytes — `salt f | wc -c`
 *     equals `wc -c < f`, always.
 *   - Markdown that reads like a document: headers, lists, quotes,
 *     emphasis, links — and fenced code blocks highlighted in their
 *     own language — while staying valid Markdown on the clipboard.
 *
 * Languages: C, sh/psh, Python, Markdown, JSON, diff, YAML, TOML,
 * JS/TS, Go, Rust, Ruby, Elixir. Detection: -l flag, else file
 * extension / well-known basename, else shebang. Unknown input
 * passes through untouched — salt degrades into cat, never into a
 * problem.
 *
 * Streaming honesty: only constructs that genuinely span lines are
 * carried as state (block comments, triple quotes, template/raw
 * strings, Markdown fences). A plain string that hits end-of-line
 * unclosed RESETS — so joining a file mid-stream (tail!) can only
 * miscolor a line, never poison the rest of the session.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>


#include "hl.h"

/* ---------------- diff ---------------- */

static void diff_line(const char *s)
{
    size_t n = strlen(s);
    if (strncmp(s, "+++", 3) == 0 || strncmp(s, "---", 3) == 0 ||
        strncmp(s, "diff ", 5) == 0 || strncmp(s, "index ", 6) == 0)
        span(s, 0, n, A_KW);
    else if (strncmp(s, "@@", 2) == 0)
        span(s, 0, n, A_HUNK);
    else if (s[0] == '+')
        span(s, 0, n, A_ADD);
    else if (s[0] == '-')
        span(s, 0, n, A_DEL);
    else
        fputs(s, stdout);
}

/* ---------------- yaml / toml ---------------- */

/* Shared tail: strings yellow, # comments grey, everything else raw. */
static void plain_values(const char *s, size_t i, size_t n)
{
    while (i < n) {
        char c = s[i];
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            span(s, i, n, A_COM);
            return;
        }
        if (c == '"' || c == '\'') {
            bool closed;
            size_t end = string_end(s, n, i + 1, c, false, &closed);
            span(s, i, end, A_STR);
            i = end;
            continue;
        }
        putchar(c);
        i++;
    }
}

static void yaml_line(const char *s)
{
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        putchar(s[i++]);
    if (i < n && s[i] == '#') {
        span(s, i, n, A_COM);
        return;
    }
    if (strncmp(s + i, "---", 3) == 0 || strncmp(s + i, "...", 3) == 0) {
        span(s, i, n, A_COM);
        return;
    }
    if (i < n && s[i] == '-' && (i + 1 == n || s[i + 1] == ' ')) {
        span(s, i, i + 1, A_BULLET);
        i++;
        while (i < n && s[i] == ' ')
            putchar(s[i++]);
    }
    /* a key runs to a ':' followed by space or end-of-line */
    size_t j = i;
    while (j < n && s[j] != ':' && s[j] != '#')
        j++;
    if (j < n && s[j] == ':' && (j + 1 == n || s[j + 1] == ' ')) {
        span(s, i, j + 1, A_KEY);
        i = j + 1;
    }
    plain_values(s, i, n);
}

static void toml_line(const char *s)
{
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        putchar(s[i++]);
    if (i < n && s[i] == '#') {
        span(s, i, n, A_COM);
        return;
    }
    if (i < n && s[i] == '[') {
        span(s, i, n, A_META); /* [section] */
        return;
    }
    size_t j = i;
    while (j < n && s[j] != '=')
        j++;
    if (j < n) {
        span(s, i, j, A_KEY);
        putchar('=');
        i = j + 1;
    }
    plain_values(s, i, n);
}

/* ---------------- markdown ---------------- */

typedef struct {
    bool in_fence;
    const spec *fence_sp; /* language inside the fence, may be NULL */
    lstate fence_ls;
} mdstate;

/* Inline pass: `code`, **bold**, *italic*, [text](url). The marker
 * characters are printed too — styled, but present: what you copy
 * is still Markdown. */
static void md_inline(const char *s, size_t i, size_t n)
{
    while (i < n) {
        char c = s[i];
        if (c == '`') {
            size_t k = i + 1;
            while (k < n && s[k] != '`')
                k++;
            if (k < n) {
                span(s, i, k + 1, A_STR);
                i = k + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c) {
            size_t k = i + 2;
            while (k + 1 < n && !(s[k] == c && s[k + 1] == c))
                k++;
            if (k + 1 < n) {
                span(s, i, k + 2, A_BOLD);
                i = k + 2;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] != c &&
            !isspace((unsigned char)s[i + 1])) {
            size_t k = i + 1;
            while (k < n && s[k] != c)
                k++;
            if (k < n) {
                span(s, i, k + 1, A_ITAL);
                i = k + 1;
                continue;
            }
        }
        if (c == '[') {
            size_t k = i + 1;
            while (k < n && s[k] != ']')
                k++;
            if (k + 1 < n && s[k + 1] == '(') {
                size_t u = k + 2;
                while (u < n && s[u] != ')')
                    u++;
                if (u < n) {
                    span(s, i, k + 1, A_LINK);
                    span(s, k + 1, u + 1, A_URL);
                    i = u + 1;
                    continue;
                }
            }
        }
        putchar(c);
        i++;
    }
}

static void md_line(mdstate *md, const char *s)
{
    size_t n = strlen(s);
    size_t t = 0;
    while (t < n && (s[t] == ' ' || s[t] == '\t'))
        t++;

    bool fence_mark = strncmp(s + t, "```", 3) == 0 ||
                      strncmp(s + t, "~~~", 3) == 0;

    if (md->in_fence) {
        if (fence_mark) {
            span(s, 0, n, A_COM);
            md->in_fence = false;
            return;
        }
        if (md->fence_sp)
            generic_line(md->fence_sp, &md->fence_ls, s);
        else
            span(s, 0, n, A_STR); /* unknown language: inked as code */
        return;
    }
    if (fence_mark) {
        span(s, 0, n, A_COM);
        md->in_fence = true;
        md->fence_ls = (lstate){ 0 };
        char lang[32] = "";
        size_t k = t + 3, w = 0;
        while (k < n && !isspace((unsigned char)s[k]) &&
               w + 1 < sizeof lang)
            lang[w++] = s[k++];
        lang[w] = '\0';
        md->fence_sp = spec_by_token(lang);
        if (md->fence_sp && md->fence_sp->kind != SP_GENERIC)
            md->fence_sp = NULL; /* no markdown-in-markdown adventures */
        return;
    }

    if (s[t] == '#') {
        size_t level = 0;
        while (s[t + level] == '#')
            level++;
        span(s, 0, n, level <= 2 ? A_H1 : A_H3);
        return;
    }
    if (s[t] == '>') {
        span(s, 0, n, A_QUOTE);
        return;
    }
    /* a horizontal rule: three or more of - _ * and nothing else */
    if (n - t >= 3 && (s[t] == '-' || s[t] == '_' || s[t] == '*')) {
        size_t k = t;
        char r = s[t];
        while (k < n && (s[k] == r || s[k] == ' '))
            k++;
        if (k == n && strchr("-_*", r)) {
            span(s, 0, n, A_COM);
            return;
        }
    }
    fwrite(s, 1, t, stdout); /* leading indent, untouched */
    size_t i = t;
    if ((s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') {
        span(s, i, i + 1, A_BULLET);
        i++;
    } else if (isdigit((unsigned char)s[i])) {
        size_t k = i;
        while (isdigit((unsigned char)s[k]))
            k++;
        if ((s[k] == '.' || s[k] == ')') && s[k + 1] == ' ') {
            span(s, i, k + 1, A_BULLET);
            i = k + 1;
        }
    }
    md_inline(s, i, n);
}

/* ---------------- driving one stream ---------------- */

static bool show_numbers;

static int salt_stream(FILE *f, const char *label, const spec *forced)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t got = getline(&line, &cap, f);
    if (got < 0) {
        free(line);
        return 0; /* empty is fine */
    }
    if (memchr(line, '\0', (size_t)got)) {
        fprintf(stderr, "salt: %s: binary — not salting that\n", label);
        free(line);
        return 1;
    }

    const spec *sp = forced;
    if (!sp && strcmp(label, "-") != 0)
        sp = spec_by_filename(label);
    if (!sp)
        sp = spec_by_shebang(line);

    lstate ls = { 0 };
    mdstate md = { 0 };
    size_t lineno = 0;

    do {
        if (memchr(line, '\0', (size_t)got)) {
            fprintf(stderr, "salt: %s: turned binary mid-stream\n", label);
            free(line);
            return 1;
        }
        bool had_nl = got > 0 && line[got - 1] == '\n';
        if (had_nl)
            line[got - 1] = '\0';

        if (show_numbers) {
            cput(A_COM);
            printf("%5zu  ", ++lineno);
            cput(A_RESET);
        }
        if (!sp || !color_on)
            fputs(line, stdout); /* cat, faithfully */
        else
            switch (sp->kind) {
            case SP_MARKDOWN: md_line(&md, line); break;
            case SP_DIFF:     diff_line(line); break;
            case SP_YAML:     yaml_line(line); break;
            case SP_TOML:     toml_line(line); break;
            case SP_GENERIC:  generic_line(sp, &ls, line); break;
            }
        if (had_nl)
            putchar('\n');
    } while ((got = getline(&line, &cap, f)) >= 0);

    free(line);
    return 0;
}

/* ---------------- main ---------------- */

static void usage(void)
{
    fputs("salt — cat with colors, faithful to the bytes 🧂\n"
          "usage: salt [-n] [-c|-p] [-l LANG] [FILE...]\n"
          "  -n       line numbers (opt-in: they DO end up in copies)\n"
          "  -c       color even when piped (for less -R)\n"
          "  -p       no color ever\n"
          "  -l LANG  force the language (name or extension)\n"
          "  -L       list languages\n"
          "With no FILE, or FILE of -, reads stdin. When stdout is\n"
          "not a terminal, output is byte-identical to the input.\n",
          stderr);
}

int main(int argc, char **argv)
{
    const spec *forced = NULL;
    bool force_color = false, no_color = false;
    int opt;

    while ((opt = getopt(argc, argv, "ncpl:Lh")) != -1) {
        switch (opt) {
        case 'n': show_numbers = true; break;
        case 'c': force_color = true; break;
        case 'p': no_color = true; break;
        case 'l':
            forced = spec_by_token(optarg);
            if (!forced) {
                fprintf(stderr, "salt: unknown language: %s (-L lists)\n",
                        optarg);
                return 2;
            }
            break;
        case 'L':
            for (size_t i = 0; i < NSPECS; i++)
                printf("%-12s %s\n", specs[i].name, specs[i].aliases);
            return 0;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }

    color_on = no_color ? false : (force_color || isatty(STDOUT_FILENO));
    setvbuf(stdout, NULL, _IOLBF, 0); /* tail -f | salt stays live */

    int status = 0;
    if (optind >= argc)
        return salt_stream(stdin, "-", forced);

    bool many = argc - optind > 1;
    for (int i = optind; i < argc; i++) {
        FILE *f;
        if (strcmp(argv[i], "-") == 0)
            f = stdin;
        else if (!(f = fopen(argv[i], "r"))) {
            perror(argv[i]);
            status = 1;
            continue;
        }
        if (many && color_on)
            printf(A_COM "── %s ──" A_RESET "\n", argv[i]);
        if (salt_stream(f, argv[i], forced))
            status = 1;
        if (f != stdin)
            fclose(f);
    }
    return status;
}
