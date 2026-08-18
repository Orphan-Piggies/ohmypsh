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
 *   The prompt + buffer occupy N terminal rows once wrapped at the
 *   terminal width. We remember which of those rows the cursor was
 *   left on (cur_row). To repaint: cursor up cur_row rows, carriage
 *   return, clear to end of screen, rewrite everything, then park the
 *   cursor by moving up from the bottom row. All positions are
 *   DISPLAY COLUMNS — computed by decoding UTF-8 and asking wcwidth,
 *   never by counting bytes (ə is one column, 🫛 is two, and \001..\002
 *   brackets mark zero-width escape sequences, readline's convention).
 *
 * The buffer itself is bytes; the cursor moves by whole codepoints.
 *
 * Opt-in for now: PSH_EDITOR=nut switches from readline to this, live
 * (it's checked every prompt). Readline stays the default until the
 * cockpit reaches feature parity (H4.4).
 */
#define _XOPEN_SOURCE 700 /* wcwidth */

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

/* Display width of the first n bytes: skips \001..\002 (escape
 * brackets) and raw CSI sequences, decodes UTF-8, asks wcwidth. */
static size_t disp_width(const char *s, size_t n)
{
    size_t w = 0, i = 0;
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
        } else {
            size_t cl = cp_len((unsigned char)s[i]);
            if (i + cl > n)
                break;
            int cw = wcwidth((wchar_t)decode_cp(s + i, cl));
            w += cw > 0 ? (size_t)cw : 1;
            i += cl;
        }
    }
    return w;
}

/* A prompt may contain newlines (themes can). Width math only cares
 * about the LAST prompt line; earlier lines just add rows. */
static void prompt_metrics(const char *p, size_t *width, size_t *newlines)
{
    const char *last = strrchr(p, '\n');
    *newlines = 0;
    for (const char *q = p; *q; q++)
        if (*q == '\n')
            (*newlines)++;
    last = last ? last + 1 : p;
    *width = disp_width(last, strlen(last));
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
    size_t pw, pnl;    /* prompt: last-line width, newline count */
    char *buf;
    size_t len, cap;   /* bytes */
    size_t pos;        /* cursor, byte offset */
    size_t cols;
    size_t cur_row;    /* row (within our paint area) cursor is on */
    size_t hist_ix;    /* == hist_n when editing a fresh line */
    char *saved;       /* the fresh line, stashed while browsing */
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

/*
 * The repaint. See the header comment for the model; the one quirk
 * handled here: when content ends EXACTLY at the right edge, the
 * terminal holds the cursor in the last column ("pending wrap"), so
 * we force the wrap with a real newline to keep the math honest.
 */
static void refresh(struct el *e)
{
    struct obuf o = { 0 };
    size_t bw = disp_width(e->buf, e->len);
    size_t cw = disp_width(e->buf, e->pos);

    if (e->cur_row > 0)
        ob_fmt(&o, "\033[%zuA", e->cur_row);
    ob_str(&o, "\r\033[J");

    for (const char *p = e->prompt; *p; p++) /* strip \001/\002 */
        if (*p != '\001' && *p != '\002')
            ob_put(&o, p, 1);
    ob_put(&o, e->buf, e->len);

    size_t end_row = e->pnl + (e->pw + bw) / e->cols;
    if ((e->pw + bw) > 0 && (e->pw + bw) % e->cols == 0)
        ob_str(&o, "\n"); /* force the pending wrap */

    size_t crow = e->pnl + (e->pw + cw) / e->cols;
    size_t ccol = (e->pw + cw) % e->cols;
    if (end_row > crow)
        ob_fmt(&o, "\033[%zuA", end_row - crow);
    ob_str(&o, "\r");
    if (ccol > 0)
        ob_fmt(&o, "\033[%zuC", ccol);

    ob_flush(&o);
    free(o.b);
    e->cur_row = crow;
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

static void handle_escape(struct el *e)
{
    if (!pending_input())
        return; /* a real Escape press: ignore */
    char a;
    if (read(STDIN_FILENO, &a, 1) != 1)
        return;
    if (a == 'b') { word_back(e); return; }
    if (a == 'f') { word_fwd(e); return; }
    if (a != '[' && a != 'O')
        return;
    char b;
    if (read(STDIN_FILENO, &b, 1) != 1)
        return;
    switch (b) {
    case 'A': if (e->hist_ix > 0) hist_to(e, e->hist_ix - 1); return;
    case 'B': if (e->hist_ix < hist_n) hist_to(e, e->hist_ix + 1); return;
    case 'C': e->pos = next_cp(e->buf, e->len, e->pos); return;
    case 'D': e->pos = prev_cp(e->buf, e->pos); return;
    case 'H': e->pos = 0; return;
    case 'F': e->pos = e->len; return;
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
                if (seq[0] == '3' && n == 1) /* Delete */
                    el_delete(e, e->pos, next_cp(e->buf, e->len, e->pos));
                else if ((seq[0] == '1' || seq[0] == '7') && n == 1)
                    e->pos = 0;
                else if ((seq[0] == '4' || seq[0] == '8') && n == 1)
                    e->pos = e->len;
            } else if (strcmp(seq, "1;5") == 0) { /* Ctrl-arrows */
                if (c == 'C') word_fwd(e);
                if (c == 'D') word_back(e);
            }
            return;
        }
    }
}

/* ---------------- the entry points ---------------- */

bool psh_editor_active(void)
{
    const char *v = psh_var_get("PSH_EDITOR");
    return v && strcmp(v, "nut") == 0 &&
           isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

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

    struct el e = { .prompt = prompt, .cols = term_cols(),
                    .hist_ix = hist_n };
    prompt_metrics(prompt, &e.pw, &e.pnl);
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
                (void)!write(STDOUT_FILENO, "^C\n", 3);
                result = strdup("");
                break;
            }
            refresh(&e);
            continue;
        }
        if (n <= 0) { /* EOF / error */
            result = e.len ? strdup(e.buf) : NULL;
            break;
        }
        switch (c) {
        case '\r':
        case '\n':
            e.pos = e.len;
            refresh(&e);
            (void)!write(STDOUT_FILENO, "\n", 1);
            result = strdup(e.buf);
            done = true;
            continue;
        case 4: /* Ctrl-D: EOF on empty, delete-char otherwise */
            if (e.len == 0) {
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
        case 5: e.pos = e.len; break;                            /* ^E */
        case 2: e.pos = prev_cp(e.buf, e.pos); break;            /* ^B */
        case 6: e.pos = next_cp(e.buf, e.len, e.pos); break;     /* ^F */
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
        case 12: /* ^L: clear screen, repaint at the top */
            (void)!write(STDOUT_FILENO, "\033[H\033[2J", 7);
            e.cur_row = 0;
            break;
        case '\t': /* completion arrives in H4.2 */
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
        refresh(&e);
    }

    sigaction(SIGWINCH, &old_winch, NULL);
    disable_raw();
    free(e.buf);
    free(e.saved);
    return result;
}
