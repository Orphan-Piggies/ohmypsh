/*
 * roast — the armory's Markdown renderer. 🫛🔥
 *
 * You salt a pistachio to keep it what it is; you roast it to turn
 * it into something else. Where salt never changes a byte, roast
 * exists to change them: it takes Markdown SOURCE and prints a
 * DOCUMENT.
 *
 *   ## Heading            →  Heading, bold green, underlined — no ##
 *   - [ ] / - [x] items   →  ☐ open / ☑ done
 *   - bullets             →  • (◦ and ▪ as they nest)
 *   **bold** *italic*     →  bold and italic, markers gone
 *   `code`                →  colored, backticks gone
 *   [text](url)           →  a real terminal hyperlink (OSC 8);
 *                            -u prints the url after it instead
 *   > quotes              →  ┃ barred, dim green
 *   ``` fences            →  fence lines dropped, the code kept,
 *                            highlighted in its own language (the
 *                            shared hl.h engine — same as salt)
 *   | tables |            →  aligned columns with real rules
 *   ---                   →  a rule across the terminal
 *
 * Still line-at-a-time (tail -f NOTES.md | roast works); the only
 * buffered construct is a table, which needs its widths before it
 * can be drawn. Roast is for reading — when you need to COPY the
 * source, that's salt's job.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "hl.h"

#define R_H     "\x1b[1;32m"  /* headings: bold green */
#define R_HDIM  "\x1b[32m"    /* deep headings: green */
#define R_RULE  "\x1b[90m"
#define R_DONE  "\x1b[32m"    /* ☑ */
#define R_TODO  "\x1b[90m"    /* ☐ */

static bool show_urls;
static int term_width;

/* Display columns of a UTF-8 string, roughly: continuation bytes are
 * free, everything else is one column. (Double-width emoji drift a
 * little; a table survives it.) */
static size_t uwidth(const char *s, size_t n)
{
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            w++;
    return w;
}

static void repeat(const char *glyph, size_t times)
{
    for (size_t i = 0; i < times; i++)
        fputs(glyph, stdout);
}

/* ---------------- inline markup ---------------- */

static void put_link(const char *text, size_t tn, const char *url,
                     size_t un)
{
    if (color_on) {
        printf("\x1b]8;;%.*s\x1b\\", (int)un, url);
        cput("\x1b[4;36m"); /* underlined cyan: it's clickable */
        fwrite(text, 1, tn, stdout);
        cput(A_RESET);
        fputs("\x1b]8;;\x1b\\", stdout);
    } else {
        fwrite(text, 1, tn, stdout);
    }
    if (show_urls || !color_on) {
        cput(A_URL);
        printf(" (%.*s)", (int)un, url);
        cput(A_RESET);
    }
}

/* Emphasis close: the marker must end a word, not start one. */
static bool closes(const char *s, size_t n, size_t i)
{
    return i + 1 >= n || !isalnum((unsigned char)s[i + 1]);
}

static void render_inline(const char *s, size_t i, size_t n)
{
    while (i < n) {
        char c = s[i];

        if (c == '\\' && i + 1 < n && strchr("\\`*_[]()#>|!-", s[i + 1])) {
            putchar(s[i + 1]); /* escaped: the character, unescaped */
            i += 2;
            continue;
        }
        if (c == '`') {
            size_t k = i + 1;
            while (k < n && s[k] != '`')
                k++;
            if (k < n) {
                span(s, i + 1, k, A_STR); /* backticks stay behind */
                i = k + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < n) {
            size_t run = 1;
            while (run < 3 && i + run < n && s[i + run] == c)
                run++;
            /* _snake_case_ protection: _ only opens after a break */
            bool can_open = i + run < n &&
                            !isspace((unsigned char)s[i + run]) &&
                            (c == '*' || i == 0 ||
                             !isalnum((unsigned char)s[i - 1]));
            if (can_open) {
                char marker[4] = { c, c, c, 0 };
                marker[run] = 0;
                const char *e = NULL;
                for (size_t k = i + run; k + run <= n; k++)
                    if (strncmp(s + k, marker, run) == 0 &&
                        !isspace((unsigned char)s[k - 1]) &&
                        (c == '*' || closes(s, n, k + run - 1))) {
                        e = s + k;
                        break;
                    }
                if (e) {
                    const char *style = run == 1 ? "\x1b[3m"
                                      : run == 2 ? "\x1b[1m"
                                                 : "\x1b[1;3m";
                    cput(style);
                    render_inline(s, i + run, (size_t)(e - s));
                    cput(A_RESET);
                    i = (size_t)(e - s) + run;
                    continue;
                }
            }
        }
        if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
            size_t b = i + (c == '!' ? 1 : 0);
            size_t k = b + 1;
            while (k < n && s[k] != ']')
                k++;
            if (k + 1 < n && s[k + 1] == '(') {
                size_t u = k + 2;
                while (u < n && s[u] != ')')
                    u++;
                if (u < n) {
                    if (c == '!')
                        fputs("🖼 ", stdout);
                    put_link(s + b + 1, k - b - 1, s + k + 2, u - k - 2);
                    i = u + 1;
                    continue;
                }
            }
        }
        putchar(c);
        i++;
    }
}

/* ---------------- tables ---------------- */

#define MAXCOLS 16

typedef struct {
    char **rows;
    size_t nrows, cap;
} table;

static void table_push(table *t, const char *line)
{
    if (t->nrows == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->rows = realloc(t->rows, t->cap * sizeof *t->rows);
        if (!t->rows) {
            fprintf(stderr, "roast: out of memory\n");
            exit(1);
        }
    }
    t->rows[t->nrows++] = strdup(line);
}

/* Split one |-row into trimmed cells. Returns the count. */
static size_t split_cells(char *row, char *cells[MAXCOLS])
{
    char *p = row;
    if (*p == '|')
        p++;
    size_t nc = 0;
    while (*p && nc < MAXCOLS) {
        char *start = p;
        while (*p && *p != '|')
            p++;
        char *end = p;
        if (*p == '|')
            p++;
        while (start < end && (*start == ' ' || *start == '\t'))
            start++;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        *end = '\0';
        cells[nc++] = start;
        if (!*p && end == start && nc > 0 && *row && p[-1] == '|')
            break; /* trailing | leaves a ghost cell — drop it */
    }
    /* a completely empty trailing cell is the line's closing pipe */
    if (nc && !*cells[nc - 1])
        nc--;
    return nc;
}

static bool is_separator_cell(const char *c)
{
    if (*c == ':')
        c++;
    if (*c != '-')
        return false;
    while (*c == '-')
        c++;
    if (*c == ':')
        c++;
    return *c == '\0';
}

static void table_flush(table *t)
{
    if (t->nrows == 0)
        return;

    /* Not actually a table? Print the lines as ordinary text. */
    bool real = false;
    if (t->nrows >= 2) {
        char *probe = strdup(t->rows[1]);
        char *cells[MAXCOLS];
        size_t nc = split_cells(probe, cells);
        real = nc > 0;
        for (size_t i = 0; i < nc && real; i++)
            real = is_separator_cell(cells[i]);
        free(probe);
    }
    if (!real) {
        for (size_t r = 0; r < t->nrows; r++) {
            render_inline(t->rows[r], 0, strlen(t->rows[r]));
            putchar('\n');
        }
        goto done;
    }

    size_t width[MAXCOLS] = { 0 };
    size_t ncols = 0;
    for (size_t r = 0; r < t->nrows; r++) {
        if (r == 1)
            continue; /* the separator row doesn't vote on widths */
        char *tmp = strdup(t->rows[r]);
        char *cells[MAXCOLS];
        size_t nc = split_cells(tmp, cells);
        if (nc > ncols)
            ncols = nc;
        for (size_t i = 0; i < nc; i++) {
            size_t w = uwidth(cells[i], strlen(cells[i]));
            if (w > width[i])
                width[i] = w;
        }
        free(tmp);
    }

    for (size_t r = 0; r < t->nrows; r++) {
        if (r == 1) { /* the rule between head and body */
            cput(R_RULE);
            for (size_t i = 0; i < ncols; i++) {
                repeat("─", width[i] + 2);
                if (i + 1 < ncols)
                    fputs("┼", stdout);
            }
            cput(A_RESET);
            putchar('\n');
            continue;
        }
        char *tmp = strdup(t->rows[r]);
        char *cells[MAXCOLS] = { 0 };
        size_t nc = split_cells(tmp, cells);
        for (size_t i = 0; i < ncols; i++) {
            const char *cell = i < nc ? cells[i] : "";
            putchar(' ');
            if (r == 0)
                cput("\x1b[1m");
            fputs(cell, stdout);
            if (r == 0)
                cput(A_RESET);
            if (i + 1 < ncols) { /* the last column ends clean */
                repeat(" ", width[i] - uwidth(cell, strlen(cell)) + 1);
                cput(R_RULE);
                fputs("│", stdout);
                cput(A_RESET);
            }
        }
        putchar('\n');
        free(tmp);
    }

done:
    for (size_t r = 0; r < t->nrows; r++)
        free(t->rows[r]);
    free(t->rows);
    t->rows = NULL;
    t->nrows = t->cap = 0;
}

/* ---------------- block structure ---------------- */

typedef struct {
    bool in_fence;
    const spec *fence_sp;
    lstate fence_ls;
    table tbl;
} rstate;

static void roast_line(rstate *st, const char *s)
{
    size_t n = strlen(s);
    size_t t = 0;
    while (t < n && (s[t] == ' ' || s[t] == '\t'))
        t++;

    bool fence_mark = strncmp(s + t, "```", 3) == 0 ||
                      strncmp(s + t, "~~~", 3) == 0;

    if (st->in_fence) {
        if (fence_mark) { /* the fence line itself is dropped */
            st->in_fence = false;
            return;
        }
        cput(R_RULE);
        fputs("  │ ", stdout);
        cput(A_RESET);
        if (st->fence_sp && color_on)
            generic_line(st->fence_sp, &st->fence_ls, s);
        else
            fputs(s, stdout);
        putchar('\n');
        return;
    }

    /* a table keeps buffering until a non-| line arrives */
    if (s[t] == '|') {
        table_push(&st->tbl, s + t);
        return;
    }
    table_flush(&st->tbl);

    if (fence_mark) {
        st->in_fence = true;
        st->fence_ls = (lstate){ 0 };
        char lang[32];
        size_t k = t + 3, w = 0;
        while (k < n && !isspace((unsigned char)s[k]) && w + 1 < sizeof lang)
            lang[w++] = s[k++];
        lang[w] = '\0';
        st->fence_sp = spec_by_token(lang);
        if (st->fence_sp && st->fence_sp->kind != SP_GENERIC)
            st->fence_sp = NULL;
        return; /* the opening fence is dropped too */
    }

    /* headings: #s dropped, text styled, top two levels underlined */
    if (s[t] == '#') {
        size_t level = 0;
        while (s[t + level] == '#')
            level++;
        size_t b = t + level;
        while (b < n && s[b] == ' ')
            b++;
        if (level <= 6 && b < n) {
            cput(level <= 3 ? R_H : R_HDIM);
            fwrite(s + b, 1, n - b, stdout);
            cput(A_RESET);
            putchar('\n');
            if (level <= 2) {
                cput(R_H);
                repeat(level == 1 ? "━" : "─", uwidth(s + b, n - b));
                cput(A_RESET);
                putchar('\n');
            }
            return;
        }
    }

    /* blockquote: every leading > becomes a bar */
    if (s[t] == '>') {
        size_t i = t;
        cput(A_QUOTE);
        while (s[i] == '>' || s[i] == ' ') {
            if (s[i] == '>')
                fputs("┃ ", stdout);
            i++;
        }
        render_inline(s, i, n);
        cput(A_RESET);
        putchar('\n');
        return;
    }

    /* a horizontal rule spans the terminal */
    if (n - t >= 3 && (s[t] == '-' || s[t] == '_' || s[t] == '*')) {
        size_t k = t;
        char r = s[t];
        bool rule = true;
        while (k < n && rule) {
            if (s[k] != r && s[k] != ' ')
                rule = false;
            k++;
        }
        if (rule && k == n) {
            cput(R_RULE);
            repeat("─", (size_t)term_width);
            cput(A_RESET);
            putchar('\n');
            return;
        }
    }

    fwrite(s, 1, t, stdout); /* keep the indent: nesting reads right */
    size_t i = t;

    /* task boxes and bullets */
    if ((s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') {
        if (strncmp(s + i + 2, "[ ] ", 4) == 0) {
            cput(R_TODO);
            fputs("☐ ", stdout);
            cput(A_RESET);
            render_inline(s, i + 6, n);
            putchar('\n');
            return;
        }
        if (strncmp(s + i + 2, "[x] ", 4) == 0 ||
            strncmp(s + i + 2, "[X] ", 4) == 0) {
            cput(R_DONE);
            fputs("☑ ", stdout);
            cput(A_RESET);
            render_inline(s, i + 6, n);
            putchar('\n');
            return;
        }
        static const char *dots[] = { "•", "◦", "▪" };
        cput(A_BULLET);
        fputs(dots[(t / 2) % 3], stdout);
        cput(A_RESET);
        render_inline(s, i + 1, n);
        putchar('\n');
        return;
    }
    if (isdigit((unsigned char)s[i])) { /* ordered lists keep numbers */
        size_t k = i;
        while (isdigit((unsigned char)s[k]))
            k++;
        if ((s[k] == '.' || s[k] == ')') && s[k + 1] == ' ') {
            span(s, i, k + 1, A_BULLET);
            render_inline(s, k + 1, n);
            putchar('\n');
            return;
        }
    }

    render_inline(s, i, n);
    putchar('\n');
}

/* ---------------- main ---------------- */

static int roast_stream(FILE *f, const char *label)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    rstate st = { 0 };

    while ((got = getline(&line, &cap, f)) >= 0) {
        if (memchr(line, '\0', (size_t)got)) {
            fprintf(stderr, "roast: %s: binary — that's not Markdown\n",
                    label);
            free(line);
            return 1;
        }
        if (got > 0 && line[got - 1] == '\n')
            line[got - 1] = '\0';
        roast_line(&st, line);
    }
    table_flush(&st.tbl);
    free(line);
    return 0;
}

static void usage(void)
{
    fputs("roast — render Markdown for reading 🔥\n"
          "usage: roast [-u] [-c|-p] [-w N] [FILE...]\n"
          "  -u    print urls after link text (default: OSC 8 hyperlinks)\n"
          "  -c    color even when piped (for less -R)\n"
          "  -p    no color\n"
          "  -w N  width for horizontal rules (default: the terminal)\n"
          "Reads stdin without FILE. Roast renders; when you need to\n"
          "copy the source, that's salt's job.\n",
          stderr);
}

int main(int argc, char **argv)
{
    bool force_color = false, no_color = false;
    int opt;

    term_width = 0;
    while ((opt = getopt(argc, argv, "ucpw:h")) != -1) {
        switch (opt) {
        case 'u': show_urls = true; break;
        case 'c': force_color = true; break;
        case 'p': no_color = true; break;
        case 'w': term_width = atoi(optarg); break;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }

    color_on = no_color ? false : (force_color || isatty(STDOUT_FILENO));
    if (term_width <= 0) {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            term_width = ws.ws_col;
        else if (getenv("COLUMNS"))
            term_width = atoi(getenv("COLUMNS"));
        if (term_width <= 0)
            term_width = 80;
    }
    setvbuf(stdout, NULL, _IOLBF, 0); /* tail -f NOTES.md | roast */

    if (optind >= argc)
        return roast_stream(stdin, "-");

    int status = 0;
    for (int i = optind; i < argc; i++) {
        FILE *f;
        if (strcmp(argv[i], "-") == 0)
            f = stdin;
        else if (!(f = fopen(argv[i], "r"))) {
            perror(argv[i]);
            status = 1;
            continue;
        }
        if (roast_stream(f, argv[i]))
            status = 1;
        if (f != stdin)
            fclose(f);
    }
    return status;
}
