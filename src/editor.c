/*
 * editor.c — the cockpit (H4): a hand-rolled raw-mode line editor.
 *
 * Why replace readline? Autosuggestions and syntax highlighting need
 * to repaint the line on every keystroke with our own colors —
 * readline owns its redisplay, we can't get in. So: termios raw mode,
 * one key at a time, and a renderer that repaints prompt + buffer
 * from scratch after each key.
 *
 * The renderer's mental model (the only hard part):
 *   Prompt + buffer occupy N terminal rows once wrapped at the
 *   terminal width (embedded newlines — themes, pasted multi-line
 *   commands — also break rows). We remember which row the cursor was
 *   left on (cur_row). To repaint: cursor up cur_row rows, carriage
 *   return, clear to end of screen, rewrite everything, then park the
 *   cursor by moving up from the bottom row. All positions are
 *   DISPLAY CELLS, computed by walking the text: decode UTF-8, ask
 *   wcwidth (ə is one column, 🫛 is two), skip \001..\002 escape
 *   brackets (readline's convention), and track the terminal's
 *   pending-wrap quirk — after a glyph lands EXACTLY in the last
 *   column, the cursor hangs there until the next glyph wraps it.
 *
 * The buffer itself is bytes; the cursor moves by whole codepoints.
 *
 * H4.2 additions: Tab completion (candidates from complete.c's shared
 * engine), Ctrl-R incremental reverse search (the search "mode" is
 * just a temporary prompt — the same renderer draws it), and
 * bracketed paste (multi-line pastes land IN the buffer instead of
 * executing line by line; Enter submits the whole thing, which the
 * lexer already speaks fluently).
 *
 * Since H4.4 the cockpit IS the shell's line editor — readline went
 * overboard, and the binary is pure MIT. (Not a tty? psh_editor_readline
 * quietly degrades to getline.)
 */
#define _XOPEN_SOURCE 700 /* wcwidth */

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "psh.h"

#define HIST_MAX 1000

/* ---------------- history ---------------- */

static char *hist[HIST_MAX];
static size_t hist_n;

void psh_editor_hist_add(const char *line)
{
    if (!*line)
        return;
    if (hist_n && strcmp(hist[hist_n - 1], line) == 0)
        return; /* consecutive duplicate */
    if (hist_n == HIST_MAX) {
        free(hist[0]);
        memmove(hist, hist + 1, (HIST_MAX - 1) * sizeof *hist);
        hist_n--;
    }
    hist[hist_n++] = strdup(line);
}

void psh_editor_hist_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        psh_editor_hist_add(line);
    }
    free(line);
    fclose(f);
}

void psh_editor_hist_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (size_t i = 0; i < hist_n; i++)
        fprintf(f, "%s\n", hist[i]);
    fclose(f);
}

/* ---------------- UTF-8, one codepoint at a time ---------------- */

static size_t cp_len(unsigned char b)
{
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1; /* stray continuation byte: step over it alone */
}

static size_t prev_cp(const char *s, size_t pos)
{
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

static size_t next_cp(const char *s, size_t len, size_t pos)
{
    if (pos >= len)
        return len;
    pos += cp_len((unsigned char)s[pos]);
    return pos > len ? len : pos;
}

static unsigned decode_cp(const char *s, size_t n)
{
    const unsigned char *u = (const unsigned char *)s;
    switch (n) {
    case 2: return ((unsigned)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    case 3: return ((unsigned)(u[0] & 0x0F) << 12) |
                   ((unsigned)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    case 4: return ((unsigned)(u[0] & 0x07) << 18) |
                   ((unsigned)(u[1] & 0x3F) << 12) |
                   ((unsigned)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
    default: return u[0];
    }
}

static size_t cp_width(const char *s, size_t cl)
{
    int w = wcwidth((wchar_t)decode_cp(s, cl));
    return w > 0 ? (size_t)w : 1;
}

/* Plain display width (no wrapping) — for laying out candidate
 * columns. Skips \001..\002 and raw CSI sequences. */
static size_t disp_width(const char *s, size_t n)
{
    size_t w = 0, i = 0;
    while (i < n && s[i]) {
        if (s[i] == '\001') {
            while (i < n && s[i] && s[i] != '\002')
                i++;
            if (i < n)
                i++;
        } else if (s[i] == '\033') {
            i++;
            while (i < n && s[i] && !((s[i] >= 'A' && s[i] <= 'Z') ||
                                      (s[i] >= 'a' && s[i] <= 'z')))
                i++;
            if (i < n)
                i++;
        } else {
            size_t cl = cp_len((unsigned char)s[i]);
            if (i + cl > n)
                break;
            w += cp_width(s + i, cl);
            i += cl;
        }
    }
    return w;
}

/* ---------------- the cell walk: where does text land? ------------ */

struct cell {
    size_t row, col;
    bool pending; /* glyph filled the last column; wrap is pending */
};

static void cell_walk(struct cell *p, size_t cols, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i]) {
        if (s[i] == '\001') { /* invisible until \002 */
            while (i < n && s[i] && s[i] != '\002')
                i++;
            if (i < n)
                i++;
        } else if (s[i] == '\033') { /* unbracketed escape: skip CSI */
            i++;
            while (i < n && s[i] && !((s[i] >= 'A' && s[i] <= 'Z') ||
                                      (s[i] >= 'a' && s[i] <= 'z')))
                i++;
            if (i < n)
                i++;
        } else if (s[i] == '\n') {
            p->row++;
            p->col = 0;
            p->pending = false;
            i++;
        } else {
            if (p->pending) {
                p->row++;
                p->col = 0;
                p->pending = false;
            }
            size_t cl = cp_len((unsigned char)s[i]);
            if (i + cl > n)
                break;
            size_t w = cp_width(s + i, cl);
            p->col += w;
            if (p->col == cols) {
                p->pending = true;
            } else if (p->col > cols) { /* wide glyph wrapped whole */
                p->row++;
                p->col = w;
            }
            i += cl;
        }
    }
}

/* Where the cursor should sit for a walk result. */
static void cell_norm(struct cell *p)
{
    if (p->pending) {
        p->row++;
        p->col = 0;
        p->pending = false;
    }
}

/* ---------------- output buffer: one write per repaint ------------ */

struct obuf {
    char *b;
    size_t len, cap;
};

static void ob_put(struct obuf *o, const char *s, size_t n)
{
    if (o->len + n > o->cap) {
        o->cap = (o->len + n) * 2 + 64;
        o->b = realloc(o->b, o->cap);
    }
    memcpy(o->b + o->len, s, n);
    o->len += n;
}

static void ob_str(struct obuf *o, const char *s) { ob_put(o, s, strlen(s)); }

static void ob_fmt(struct obuf *o, const char *fmt, size_t n)
{
    char tmp[32];
    snprintf(tmp, sizeof tmp, fmt, n);
    ob_str(o, tmp);
}

static void ob_flush(struct obuf *o)
{
    if (o->len)
        (void)!write(STDOUT_FILENO, o->b, o->len);
    o->len = 0;
}

/* ---------------- raw mode ---------------- */

static struct termios orig_termios;
static bool raw_active;

/* TCSADRAIN, not TCSAFLUSH: flushing would DISCARD type-ahead — the
 * next command you started typing while the last one still ran. */
static void disable_raw(void)
{
    if (raw_active) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &orig_termios);
        raw_active = false;
    }
}

static bool enable_raw(void)
{
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0)
        return false;
    struct termios raw = orig_termios;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | INPCK | ISTRIP | IXON | ICRNL);
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN);
    /* ISIG stays ON: Ctrl-C reaches the shell's flag handler and we
     * see EINTR; Ctrl-Z at the prompt is already ignored (jobs.c). */
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) < 0)
        return false;
    raw_active = true;
    static bool at_exit;
    if (!at_exit) { /* never leave the terminal raw, whatever happens */
        atexit(disable_raw);
        at_exit = true;
    }
    return true;
}

static volatile sig_atomic_t winched;
static void on_winch(int sig) { (void)sig; winched = 1; }

static size_t term_cols(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/* ---------------- the editor state ---------------- */

struct el {
    const char *prompt;
    char *buf;
    size_t len, cap;   /* bytes */
    size_t pos;        /* cursor, byte offset */
    size_t cols;
    size_t cur_row;    /* row (within our paint area) cursor is on */
    size_t hist_ix;    /* == hist_n when editing a fresh line */
    char *saved;       /* the fresh line, stashed while browsing */
    char *sugg;        /* grey autosuggestion tail (H4.3), or NULL */
};

static void el_ensure(struct el *e, size_t extra)
{
    if (e->len + extra + 1 > e->cap) {
        e->cap = (e->len + extra + 1) * 2;
        e->buf = realloc(e->buf, e->cap);
    }
}

static void el_set(struct el *e, const char *s)
{
    size_t n = strlen(s);
    el_ensure(e, n);
    memcpy(e->buf, s, n + 1);
    e->len = e->pos = n;
}

static void el_insert(struct el *e, const char *s, size_t n)
{
    el_ensure(e, n);
    memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos);
    memcpy(e->buf + e->pos, s, n);
    e->len += n;
    e->pos += n;
    e->buf[e->len] = '\0';
}

static void el_delete(struct el *e, size_t from, size_t to)
{
    memmove(e->buf + from, e->buf + to, e->len - to);
    e->len -= to - from;
    e->pos = from;
    e->buf[e->len] = '\0';
}

/* ---------------- syntax highlighting (H4.3) ---------------- */

/*
 * The buffer is colorized by the REAL lexer — no second grammar to
 * drift out of sync. psh_tokenize gives tokens with byte offsets;
 * we copy the source verbatim, wrapping token spans in colors:
 *
 *   command position   green if it exists (builtin/function/$PATH),
 *                      red if it doesn't — typos glow before Enter
 *   keywords           bold (if/then/fi/while/for/case/{...})
 *   'str' "str"        yellow    $word      cyan    NAME=v   cyan
 *   # comments         grey (they live between tokens)
 *
 * Incomplete input (open quote, open $( ) tokenizes to nothing —
 * painted plain until it closes. Whole-word granularity, on purpose.
 */
#define C_CMD_OK  "\033[32m"
#define C_CMD_BAD "\033[31m"
#define C_KEYWORD "\033[1m"
#define C_STRING  "\033[33m"
#define C_VAR     "\033[36m"
#define C_GREY    "\033[90m"
#define C_OFF     "\033[0m"

static bool is_keyword(const char *w)
{
    static const char *kw[] = { "if", "elif", "else", "fi", "then",
                                "while", "for", "do", "done", "case",
                                "esac", "in", "{", "}", NULL };
    for (size_t i = 0; kw[i]; i++)
        if (strcmp(w, kw[i]) == 0)
            return true;
    return false;
}

static bool is_assignment(const char *w)
{
    if (!isalpha((unsigned char)w[0]) && w[0] != '_')
        return false;
    size_t i = 1;
    while (isalnum((unsigned char)w[i]) || w[i] == '_')
        i++;
    return w[i] == '=';
}

/* One-entry cache: repaints outnumber buffer changes. */
static bool valid_command(const char *name)
{
    static char cached[256];
    static bool cached_ok;
    if (cached[0] && strcmp(cached, name) == 0)
        return cached_ok;
    bool ok = psh_find_builtin(name) || psh_function_exists(name);
    if (!ok) {
        char *p = psh_path_lookup(name);
        ok = p != NULL;
        free(p);
    }
    if (strlen(name) < sizeof cached) {
        strcpy(cached, name);
        cached_ok = ok;
    }
    return ok;
}

/* Bytes a token occupied in the source. Words copy 1:1, so text
 * length is source length; operators know their own width. */
static size_t tok_srclen(const psh_token *t)
{
    switch (t->type) {
    case TOK_WORD: return strlen(t->text);
    case TOK_AND: case TOK_OR: case TOK_DSEMI:
    case TOK_REDIR_APPEND: case TOK_REDIR_ERR: return 2;
    default: return 1;
    }
}

/* Copy the stretch BETWEEN tokens: whitespace, and comments (the
 * lexer eats those, so a '#' here starts one — grey to gap's end;
 * a comment can't cross a newline, newlines are tokens). */
static void put_gap(struct obuf *o, const char *src, size_t a, size_t b)
{
    const char *h = b > a ? memchr(src + a, '#', b - a) : NULL;
    if (!h) {
        ob_put(o, src + a, b - a);
        return;
    }
    size_t hp = (size_t)(h - src);
    ob_put(o, src + a, hp - a);
    ob_str(o, C_GREY);
    ob_put(o, src + hp, b - hp);
    ob_str(o, C_OFF);
}

static char *colorize(const char *src, size_t len)
{
    bool err = false, incomplete = false;
    psh_token *toks = psh_tokenize(src, &err, &incomplete);
    if (!toks)
        return NULL; /* blank, incomplete or error: paint plain */

    struct obuf o = { 0 };
    size_t prev = 0;
    bool cmdpos = true;
    int expect_in = 0; /* for/case: a name, then the keyword `in` */

    for (psh_token *t = toks; t; t = t->next) {
        put_gap(&o, src, prev, t->pos);
        size_t tl = tok_srclen(t);

        if (t->type == TOK_WORD) {
            const char *col = NULL;
            if (t->next && t->next->type == TOK_LPAREN) {
                col = C_VAR; /* name() — a function being born */
                cmdpos = false;
            } else if (cmdpos) {
                if (is_keyword(t->text)) {
                    col = C_KEYWORD;
                    if (strcmp(t->text, "for") == 0 ||
                        strcmp(t->text, "case") == 0) {
                        cmdpos = false;
                        expect_in = 2;
                    }
                } else if (is_assignment(t->text)) {
                    col = C_VAR; /* stays cmdpos: A=1 cmd */
                } else {
                    col = valid_command(t->text) ? C_CMD_OK : C_CMD_BAD;
                    cmdpos = false;
                }
            } else if (expect_in) {
                expect_in--;
                if (strcmp(t->text, "in") == 0) {
                    col = C_KEYWORD;
                    expect_in = 0;
                }
            } else if (t->text[0] == '\'' || t->text[0] == '"') {
                col = C_STRING;
            } else if (t->text[0] == '$') {
                col = C_VAR;
            }
            if (col) {
                ob_str(&o, col);
                ob_put(&o, src + t->pos, tl);
                ob_str(&o, C_OFF);
            } else {
                ob_put(&o, src + t->pos, tl);
            }
        } else {
            ob_put(&o, src + t->pos, tl);
            switch (t->type) {
            case TOK_PIPE: case TOK_AND: case TOK_OR: case TOK_SEMI:
            case TOK_DSEMI: case TOK_AMP: case TOK_NEWLINE:
            case TOK_RPAREN:
                cmdpos = true; /* a fresh command starts */
                expect_in = 0;
                break;
            default: /* redirects, '(' — a filename or body follows */
                break;
            }
        }
        prev = t->pos + tl;
    }
    put_gap(&o, src, prev, len);
    psh_tokens_free(toks);
    ob_put(&o, "", 1); /* NUL-terminate */
    return o.b;
}

/* ---------------- autosuggestions (H4.3) ---------------- */

/* The newest history entry the buffer is a strict prefix of; its
 * remainder is painted grey after the line. */
static void update_sugg(struct el *e)
{
    free(e->sugg);
    e->sugg = NULL;
    if (e->len == 0)
        return;
    for (size_t i = hist_n; i-- > 0;) {
        if (strncmp(hist[i], e->buf, e->len) == 0 && hist[i][e->len]) {
            e->sugg = strdup(hist[i] + e->len);
            return;
        }
    }
}

/* → / End / ^E take the whole suggestion; Alt-f / Ctrl-→ take one
 * word of it. Only when the cursor is at the end of the line. */
static bool sugg_accept(struct el *e, bool word_only)
{
    if (!e->sugg || !*e->sugg || e->pos != e->len)
        return false;
    size_t n = strlen(e->sugg);
    if (word_only) {
        size_t k = 0;
        while (e->sugg[k] == ' ')
            k++;
        while (e->sugg[k] && e->sugg[k] != ' ')
            k++;
        if (e->sugg[k] == ' ')
            k++;
        n = k;
    }
    el_insert(e, e->sugg, n);
    return true;
}

/* The repaint. See the header comment for the model. */
static void refresh(struct el *e)
{
    struct obuf o = { 0 };
    struct cell cur = { 0 }, end;
    cell_walk(&cur, e->cols, e->prompt, strlen(e->prompt));
    cell_walk(&cur, e->cols, e->buf, e->pos);
    end = cur;
    cell_walk(&end, e->cols, e->buf + e->pos, e->len - e->pos);
    if (e->sugg) /* the grey tail occupies real cells */
        cell_walk(&end, e->cols, e->sugg, strlen(e->sugg));
    cell_norm(&cur);

    if (e->cur_row > 0)
        ob_fmt(&o, "\033[%zuA", e->cur_row);
    ob_str(&o, "\r\033[J");

    for (const char *p = e->prompt; *p; p++) /* strip \001/\002 */
        if (*p != '\001' && *p != '\002')
            ob_put(&o, p, 1);
    char *colored = colorize(e->buf, e->len);
    if (colored) {
        ob_str(&o, colored);
        free(colored);
    } else {
        ob_put(&o, e->buf, e->len);
    }
    if (e->sugg) {
        ob_str(&o, C_GREY);
        ob_str(&o, e->sugg);
        ob_str(&o, C_OFF);
    }

    size_t final_row = end.row;
    if (end.pending) { /* content ends flush right: force the wrap */
        ob_str(&o, "\n");
        final_row++;
    }
    if (final_row > cur.row)
        ob_fmt(&o, "\033[%zuA", final_row - cur.row);
    ob_str(&o, "\r");
    if (cur.col > 0)
        ob_fmt(&o, "\033[%zuC", cur.col);

    ob_flush(&o);
    free(o.b);
    e->cur_row = cur.row;
}

/* Rows the full paint occupies — for stepping below it. */
static size_t paint_rows(struct el *e)
{
    struct cell c = { 0 };
    cell_walk(&c, e->cols, e->prompt, strlen(e->prompt));
    cell_walk(&c, e->cols, e->buf, e->len);
    if (e->sugg)
        cell_walk(&c, e->cols, e->sugg, strlen(e->sugg));
    return c.row + (c.pending ? 1 : 0);
}

/* ---------------- keys ---------------- */

/* After a lone ESC byte: is more input already here? (distinguishes
 * an escape sequence from the user actually pressing Escape) */
static bool pending_input(void)
{
    struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
    return poll(&p, 1, 25) > 0;
}

static void word_back(struct el *e)
{
    while (e->pos > 0 && e->buf[prev_cp(e->buf, e->pos)] == ' ')
        e->pos = prev_cp(e->buf, e->pos);
    while (e->pos > 0 && e->buf[prev_cp(e->buf, e->pos)] != ' ')
        e->pos = prev_cp(e->buf, e->pos);
}

static void word_fwd(struct el *e)
{
    while (e->pos < e->len && e->buf[e->pos] == ' ')
        e->pos = next_cp(e->buf, e->len, e->pos);
    while (e->pos < e->len && e->buf[e->pos] != ' ')
        e->pos = next_cp(e->buf, e->len, e->pos);
}

static void hist_to(struct el *e, size_t ix)
{
    if (ix == e->hist_ix)
        return;
    if (e->hist_ix == hist_n) { /* leaving the fresh line: stash it */
        free(e->saved);
        e->saved = strdup(e->buf);
    }
    e->hist_ix = ix;
    el_set(e, ix == hist_n ? (e->saved ? e->saved : "") : hist[ix]);
}

/* ---------------- bracketed paste ---------------- */

/*
 * Everything between ESC[200~ and ESC[201~ arrived as ONE paste: it
 * goes into the buffer instead of executing line by line — that's
 * the entire point. \r becomes \n (terminals paste newlines as \r),
 * other control bytes are dropped, trailing newlines stripped so a
 * copied line doesn't fire on landing. Enter submits the whole thing;
 * multi-line input is already the lexer's native language.
 */
static void read_paste(struct el *e)
{
    static const char terminator[] = "\033[201~";
    char *pb = NULL;
    size_t n = 0, cap = 0, m = 0;
    for (;;) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        if (c == terminator[m]) {
            if (++m == sizeof terminator - 1)
                break;
            continue;
        }
        if (n + m + 2 > cap) {
            cap = (n + m + 2) * 2 + 64;
            pb = realloc(pb, cap);
        }
        memcpy(pb + n, terminator, m); /* partial match was literal */
        n += m;
        m = (c == terminator[0]) ? 1 : 0;
        if (m == 0)
            pb[n++] = c;
    }
    if (!pb)
        return;

    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        char c = pb[i] == '\r' ? '\n' : pb[i];
        if ((unsigned char)c >= 32 || c == '\n' || c == '\t' ||
            (unsigned char)c >= 0x80)
            pb[out++] = c;
    }
    while (out > 0 && pb[out - 1] == '\n')
        out--;
    el_insert(e, pb, out);
    free(pb);
}

static void handle_escape(struct el *e)
{
    if (!pending_input())
        return; /* a real Escape press: ignore */
    char a;
    if (read(STDIN_FILENO, &a, 1) != 1)
        return;
    if (a == 'b') { word_back(e); return; }
    if (a == 'f') { if (!sugg_accept(e, true)) word_fwd(e); return; }
    if (a != '[' && a != 'O')
        return;
    char b;
    if (read(STDIN_FILENO, &b, 1) != 1)
        return;
    switch (b) {
    case 'A': if (e->hist_ix > 0) hist_to(e, e->hist_ix - 1); return;
    case 'B': if (e->hist_ix < hist_n) hist_to(e, e->hist_ix + 1); return;
    case 'C':
        if (!sugg_accept(e, false))
            e->pos = next_cp(e->buf, e->len, e->pos);
        return;
    case 'D': e->pos = prev_cp(e->buf, e->pos); return;
    case 'H': e->pos = 0; return;
    case 'F':
        if (!sugg_accept(e, false))
            e->pos = e->len;
        return;
    }
    if (b >= '0' && b <= '9') { /* ESC [ <digits> (;<digits>)* <final> */
        char seq[8] = { b };
        size_t n = 1;
        char c;
        while (n < sizeof seq - 1 && read(STDIN_FILENO, &c, 1) == 1) {
            if (c == ';' || (c >= '0' && c <= '9')) {
                seq[n++] = c;
                continue;
            }
            seq[n] = '\0';
            if (c == '~') {
                if (strcmp(seq, "3") == 0) /* Delete */
                    el_delete(e, e->pos, next_cp(e->buf, e->len, e->pos));
                else if (strcmp(seq, "1") == 0 || strcmp(seq, "7") == 0)
                    e->pos = 0;
                else if (strcmp(seq, "4") == 0 || strcmp(seq, "8") == 0) {
                    if (!sugg_accept(e, false))
                        e->pos = e->len;
                }
                else if (strcmp(seq, "200") == 0)
                    read_paste(e);
            } else if (strcmp(seq, "1;5") == 0) { /* Ctrl-arrows */
                if (c == 'C' && !sugg_accept(e, true)) word_fwd(e);
                if (c == 'D') word_back(e);
            }
            return;
        }
    }
}

/* ---------------- tab completion ---------------- */

static bool word_break_char(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '|' || c == ';' ||
           c == '&' || c == '<' || c == '>' || c == '(' || c == ')';
}

/* Print candidates in columns below the line, then repaint fresh. */
#define LIST_CAP 120
static void list_candidates(struct el *e, char **v, size_t n)
{
    struct obuf o = { 0 };
    size_t below = paint_rows(e) - e->cur_row;
    if (below > 0)
        ob_fmt(&o, "\033[%zuB", below);
    ob_str(&o, "\r\n");

    size_t shown = n > LIST_CAP ? LIST_CAP : n;
    size_t maxw = 0;
    for (size_t i = 0; i < shown; i++) {
        size_t w = disp_width(v[i], strlen(v[i]));
        if (w > maxw)
            maxw = w;
    }
    maxw += 2;
    size_t percol = e->cols / maxw;
    if (percol == 0)
        percol = 1;
    for (size_t i = 0; i < shown; i++) {
        ob_str(&o, v[i]);
        if ((i + 1) % percol == 0 || i + 1 == shown) {
            ob_str(&o, "\r\n");
        } else {
            size_t pad = maxw - disp_width(v[i], strlen(v[i]));
            for (size_t k = 0; k < pad; k++)
                ob_str(&o, " ");
        }
    }
    if (n > shown) {
        char more[64];
        snprintf(more, sizeof more, "… and %zu more\r\n", n - shown);
        ob_str(&o, more);
    }
    ob_flush(&o);
    free(o.b);
    e->cur_row = 0; /* we're on a fresh row; repaint from here */
    refresh(e);
}

static void do_complete(struct el *e)
{
    size_t ws = e->pos;
    while (ws > 0 && !word_break_char(e->buf[ws - 1]))
        ws--;

    /* Command position? Nothing but blanks since the last operator. */
    bool cmdpos = true;
    for (size_t k = ws; k > 0;) {
        k--;
        char ch = e->buf[k];
        if (ch == ' ' || ch == '\t')
            continue;
        cmdpos = (ch == '|' || ch == ';' || ch == '&' || ch == '\n' ||
                  ch == '(');
        break;
    }

    char *word = strndup(e->buf + ws, e->pos - ws);
    if (!word)
        return;
    size_t base_off = 0;
    char **v = cmdpos ? psh_complete_commands(word)
                      : psh_complete_files(word, &base_off);
    if (!v) {
        free(word);
        return;
    }
    size_t n = 0;
    while (v[n])
        n++;

    const char *base = word + (cmdpos ? 0 : base_off);
    size_t blen = strlen(base);

    size_t lcp = strlen(v[0]); /* longest common prefix */
    for (size_t i = 1; i < n; i++) {
        size_t k = 0;
        while (k < lcp && v[i][k] == v[0][k])
            k++;
        lcp = k;
    }
    while (lcp > blen && ((unsigned char)v[0][lcp] & 0xC0) == 0x80)
        lcp--; /* never cut a codepoint in half */

    if (n == 1) {
        el_insert(e, v[0] + blen, strlen(v[0]) - blen);
        if (v[0][strlen(v[0]) - 1] != '/')
            el_insert(e, " ", 1); /* dirs stay open for more */
    } else if (lcp > blen) {
        el_insert(e, v[0] + blen, lcp - blen);
    } else {
        list_candidates(e, v, n);
    }
    psh_complete_free(v);
    free(word);
}

/* ---------------- ^X^E: edit the line in $EDITOR ------------------ */

/*
 * Hand the buffer to $VISUAL/$EDITOR via a temp file and take back
 * whatever they saved (zsh-style: it returns TO THE BUFFER, and Enter
 * submits — bash's execute-on-exit is a touch too much trust in vim
 * muscle memory). The editor runs through psh_run_string, i.e. the
 * shell's own pipeline machinery — job control, signals and terminal
 * handoff all behave exactly as for a typed command.
 */
static void do_external_edit(struct el *e)
{
    const char *ed = psh_var_get("VISUAL");
    if (!ed || !*ed)
        ed = psh_var_get("EDITOR");
    if (!ed || !*ed)
        ed = "vi";

    char path[] = "/tmp/psh-edit-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return;
    (void)!write(fd, e->buf, e->len);
    (void)!write(fd, "\n", 1);
    close(fd);

    /* step out of the cockpit: paste-mode off, cooked mode, new row */
    (void)!write(STDOUT_FILENO, "\033[?2004l\n", 9);
    disable_raw();

    char *cmd = malloc(strlen(ed) + sizeof path + 2);
    if (cmd) {
        sprintf(cmd, "%s %s", ed, path);
        int saved_status = psh_last_status;
        psh_run_string(cmd);
        psh_last_status = saved_status; /* editing never touches $? */
        free(cmd);

        FILE *f = fopen(path, "r");
        if (f) {
            char *nb = NULL;
            size_t n = 0, cap = 0, got;
            char chunk[4096];
            while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
                if (n + got + 1 > cap) {
                    cap = (n + got + 1) * 2;
                    nb = realloc(nb, cap);
                }
                memcpy(nb + n, chunk, got);
                n += got;
            }
            fclose(f);
            if (nb) {
                while (n > 0 && nb[n - 1] == '\n')
                    n--;
                nb[n] = '\0';
                el_set(e, nb);
                free(nb);
            }
        }
    }
    unlink(path);

    enable_raw();
    (void)!write(STDOUT_FILENO, "\033[?2004h", 8);
    e->cur_row = 0; /* repaint fresh wherever the editor left us */
}

/* ---------------- Ctrl-R: incremental reverse search -------------- */

/* Scan history newest→oldest starting at `start` for `q`; on a hit,
 * show it (cursor on the match) and record where. */
static bool search_show(struct el *e, size_t start, const char *q,
                        size_t *ix)
{
    if (hist_n == 0 || !*q)
        return false;
    for (size_t i = start + 1; i-- > 0;) {
        const char *hit = strstr(hist[i], q);
        if (hit) {
            size_t off = (size_t)(hit - hist[i]);
            *ix = i;
            el_set(e, hist[i]);
            e->pos = off;
            return true;
        }
    }
    return false;
}

/* Returns 0 = keep editing, 1 = accept (execute), 2 = ^C abort. */
static int do_search(struct el *e)
{
    char q[128] = "";
    size_t ql = 0;
    size_t ix = hist_n; /* current match; hist_n = none yet */
    free(e->sugg);      /* no grey tail while searching */
    e->sugg = NULL;
    char *orig = strdup(e->buf);
    size_t orig_pos = e->pos;
    const char *saved_prompt = e->prompt;
    char sp[192];
    bool failed = false;
    int verdict = 0;

    for (;;) {
        snprintf(sp, sizeof sp, "(%sreverse-i-search)`%s': ",
                 failed ? "failed " : "", q);
        e->prompt = sp;
        refresh(e);

        unsigned char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r < 0 && errno == EINTR) {
            if (winched) {
                winched = 0;
                e->cols = term_cols();
                e->cur_row = 0;
                (void)!write(STDOUT_FILENO, "\r\033[J", 4);
            }
            if (psh_interrupted) {
                verdict = 2;
                break;
            }
            continue;
        }
        if (r <= 0)
            break;

        if (c == 18) { /* ^R again: strictly older */
            if (ix == hist_n)
                failed = !search_show(e, hist_n - 1, q, &ix);
            else if (ix == 0)
                failed = true;
            else
                failed = !search_show(e, ix - 1, q, &ix);
        } else if (c == 7) { /* ^G: cancel, restore the original */
            el_set(e, orig);
            e->pos = orig_pos;
            break;
        } else if (c == '\r' || c == '\n') {
            verdict = 1;
            break;
        } else if (c == 127 || c == 8) {
            if (ql > 0) {
                q[--ql] = '\0';
                failed = *q ? !search_show(e, hist_n - 1, q, &ix) : false;
                if (!*q) {
                    el_set(e, orig);
                    e->pos = orig_pos;
                    ix = hist_n;
                }
            }
        } else if (c == '\033') {
            if (pending_input()) { /* swallow the sequence, then leave */
                char junk;
                while (pending_input() && read(STDIN_FILENO, &junk, 1) == 1)
                    ;
            }
            break; /* ESC (or any arrow): accept what's shown, edit on */
        } else if (c >= 32 || c >= 0x80) {
            char seq[4] = { (char)c };
            size_t need = cp_len(c);
            for (size_t i = 1; i < need; i++)
                if (read(STDIN_FILENO, seq + i, 1) != 1) {
                    need = i;
                    break;
                }
            if (ql + need < sizeof q) {
                memcpy(q + ql, seq, need);
                ql += need;
                q[ql] = '\0';
                /* stay on the current match if it still fits */
                failed = !search_show(
                    e, ix == hist_n ? hist_n - 1 : ix, q, &ix);
            }
        } else {
            break; /* any other control key: accept and edit on */
        }
    }

    e->prompt = saved_prompt;
    free(orig);
    return verdict;
}

/* ---------------- the entry points ---------------- */

char *psh_editor_readline(const char *prompt)
{
    if (!enable_raw()) { /* not a tty after all: plain line read */
        char *line = NULL;
        size_t cap = 0;
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            free(line);
            return NULL;
        }
        if (n > 0 && line[n - 1] == '\n')
            line[n - 1] = '\0';
        return line;
    }

    struct sigaction sa = { 0 }, old_winch;
    sa.sa_handler = on_winch; /* no SA_RESTART: read must EINTR */
    sigaction(SIGWINCH, &sa, &old_winch);
    (void)!write(STDOUT_FILENO, "\033[?2004h", 8); /* bracketed paste */

    struct el e = { .prompt = prompt, .cols = term_cols(),
                    .hist_ix = hist_n };
    el_ensure(&e, 64);
    e.buf[0] = '\0';
    refresh(&e);

    char *result = NULL;
    bool done = false;
    while (!done) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) {
            if (winched) {
                winched = 0;
                e.cols = term_cols();
                e.cur_row = 0; /* the resize reflowed us; start clean */
                (void)!write(STDOUT_FILENO, "\r\033[J", 4);
            }
            if (psh_interrupted) { /* Ctrl-C: abandon the line */
                psh_interrupted = 0;
                (void)!write(STDOUT_FILENO, "^C\033[?2004l\n", 11);
                result = strdup("");
                break;
            }
            refresh(&e);
            continue;
        }
        if (n <= 0) { /* EOF / error */
            (void)!write(STDOUT_FILENO, "\033[?2004l", 8);
            result = e.len ? strdup(e.buf) : NULL;
            break;
        }
        switch (c) {
        case '\r':
        case '\n':
submit:
            e.pos = e.len;
            free(e.sugg); /* the grey tail must not outlive the line */
            e.sugg = NULL;
            refresh(&e);
            /* paste-mode off BEFORE the newline hands the screen to
             * the command — nothing may leak into its output line */
            (void)!write(STDOUT_FILENO, "\033[?2004l\n", 9);
            result = strdup(e.buf);
            done = true;
            continue;
        case 4: /* Ctrl-D: EOF on empty, delete-char otherwise */
            if (e.len == 0) {
                (void)!write(STDOUT_FILENO, "\033[?2004l", 8);
                done = true;
                continue;
            }
            el_delete(&e, e.pos, next_cp(e.buf, e.len, e.pos));
            break;
        case 127:
        case 8: /* Backspace */
            if (e.pos > 0)
                el_delete(&e, prev_cp(e.buf, e.pos), e.pos);
            break;
        case 1: e.pos = 0; break;                                /* ^A */
        case 5: /* ^E: end — or take the suggestion, fish-style */
            if (!sugg_accept(&e, false))
                e.pos = e.len;
            break;
        case 2: e.pos = prev_cp(e.buf, e.pos); break;            /* ^B */
        case 6: /* ^F: right — or take the suggestion at the edge */
            if (!sugg_accept(&e, false))
                e.pos = next_cp(e.buf, e.len, e.pos);
            break;
        case 11: e.len = e.pos; e.buf[e.len] = '\0'; break;      /* ^K */
        case 21: el_delete(&e, 0, e.pos); break;                 /* ^U */
        case 23: {                                               /* ^W */
            size_t end = e.pos;
            word_back(&e);
            el_delete(&e, e.pos, end);
            break;
        }
        case 16: if (e.hist_ix > 0) hist_to(&e, e.hist_ix - 1);  /* ^P */
            break;
        case 14: if (e.hist_ix < hist_n) hist_to(&e, e.hist_ix + 1);
            break;                                               /* ^N */
        case 18: {                                               /* ^R */
            int verdict = do_search(&e);
            if (verdict == 1)
                goto submit;
            if (verdict == 2) {
                psh_interrupted = 0;
                (void)!write(STDOUT_FILENO, "^C\033[?2004l\n", 11);
                result = strdup("");
                done = true;
                continue;
            }
            break;
        }
        case 20: {                                               /* ^T */
            if (e.len < 2 || e.pos == 0)
                break;
            size_t p2 = (e.pos == e.len) ? prev_cp(e.buf, e.len) : e.pos;
            size_t p1 = prev_cp(e.buf, p2);
            size_t p3 = next_cp(e.buf, e.len, p2);
            char tmp[8];
            memcpy(tmp, e.buf + p1, p2 - p1);
            memmove(e.buf + p1, e.buf + p2, p3 - p2);
            memcpy(e.buf + p1 + (p3 - p2), tmp, p2 - p1);
            e.pos = p3;
            break;
        }
        case 24: { /* ^X: chord prefix */
            unsigned char c2;
            if (read(STDIN_FILENO, &c2, 1) == 1 && c2 == 5)
                do_external_edit(&e); /* ^X^E */
            break;
        }
        case 12: /* ^L: clear screen, repaint at the top */
            (void)!write(STDOUT_FILENO, "\033[H\033[2J", 7);
            e.cur_row = 0;
            break;
        case '\t':
            do_complete(&e);
            break;
        case '\033':
            handle_escape(&e);
            break;
        default:
            if (c >= 32 || c >= 0x80) { /* printable or UTF-8 lead */
                char seq[4] = { (char)c };
                size_t need = cp_len(c);
                for (size_t i = 1; i < need; i++)
                    if (read(STDIN_FILENO, seq + i, 1) != 1) {
                        need = i;
                        break;
                    }
                el_insert(&e, seq, need);
            }
            break;
        }
        update_sugg(&e);
        refresh(&e);
    }

    sigaction(SIGWINCH, &old_winch, NULL);
    disable_raw();
    free(e.buf);
    free(e.saved);
    free(e.sugg);
    return result;
}
