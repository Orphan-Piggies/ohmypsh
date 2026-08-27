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

/* ---------------- palette ---------------- */

#define A_RESET   "\x1b[0m"
#define A_KW      "\x1b[1m"      /* keywords: bold */
#define A_TYPE    "\x1b[35m"     /* types / constants: magenta */
#define A_STR     "\x1b[33m"     /* strings: yellow */
#define A_COM     "\x1b[90m"     /* comments: grey */
#define A_NUM     "\x1b[36m"     /* numbers: cyan */
#define A_VAR     "\x1b[36m"     /* $variables: cyan */
#define A_META    "\x1b[35m"     /* preprocessor, @deco, :atoms */
#define A_KEY     "\x1b[36m"     /* yaml/toml/json keys: cyan */
#define A_H1      "\x1b[1;32m"   /* markdown h1/h2: bold green */
#define A_H3      "\x1b[32m"     /* markdown h3+: green */
#define A_BULLET  "\x1b[32m"
#define A_QUOTE   "\x1b[2;32m"   /* blockquote: dim green */
#define A_LINK    "\x1b[36m"
#define A_URL     "\x1b[90m"
#define A_BOLD    "\x1b[1m"
#define A_ITAL    "\x1b[3m"
#define A_ADD     "\x1b[32m"
#define A_DEL     "\x1b[31m"
#define A_HUNK    "\x1b[36m"

static bool color_on;

static void cput(const char *code)
{
    if (color_on)
        fputs(code, stdout);
}

/* ---------------- language specs ---------------- */

typedef enum { SP_GENERIC, SP_MARKDOWN, SP_YAML, SP_TOML, SP_DIFF } sp_kind;

typedef struct {
    const char *name;
    const char *aliases; /* space-separated names/extensions/basenames */
    sp_kind kind;
    const char **kw;     /* NULL-terminated; bold */
    const char **kw2;    /* types & named constants; magenta */
    const char *lcom;    /* line comment token, "" for none */
    bool lcom_boundary;  /* sh-style: # comments only after whitespace */
    const char *bopen;   /* block comment tokens, NULL for none */
    const char *bclose;
    const char *quotes;  /* string-opening characters */
    bool triple;         /* python/elixir """ spans lines */
    bool backtick_multi; /* js template / go raw strings span lines */
    bool dollar_var;     /* $name / ${...} */
    bool at_word;        /* @word (decorators, ivars, attrs) */
    bool colon_sym;      /* :word (ruby symbols, elixir atoms) */
    bool hash_preproc;   /* C: leading # is a directive */
    bool json_keys;      /* "string": colored as a key */
} spec;

static const char *c_kw[] = { "auto", "break", "case", "const", "continue",
    "default", "do", "else", "enum", "extern", "for", "goto", "if",
    "inline", "register", "restrict", "return", "sizeof", "static",
    "struct", "switch", "typedef", "union", "volatile", "while", NULL };
static const char *c_kw2[] = { "bool", "char", "double", "float", "int",
    "long", "short", "signed", "unsigned", "void", "true", "false",
    "NULL", "size_t", "ssize_t", "pid_t", "FILE", NULL };

static const char *sh_kw[] = { "if", "then", "else", "elif", "fi", "while",
    "until", "do", "done", "for", "in", "case", "esac", "function",
    "local", "return", "break", "continue", "export", "unset", "source",
    "exec", "read", "echo", "cd", "set", "trap", "exit", "shift", NULL };

static const char *py_kw[] = { "False", "None", "True", "and", "as",
    "assert", "async", "await", "break", "class", "continue", "def",
    "del", "elif", "else", "except", "finally", "for", "from", "global",
    "if", "import", "in", "is", "lambda", "nonlocal", "not", "or",
    "pass", "raise", "return", "try", "while", "with", "yield", NULL };
static const char *py_kw2[] = { "self", "cls", NULL };

static const char *js_kw[] = { "async", "await", "break", "case", "catch",
    "class", "const", "continue", "default", "delete", "do", "else",
    "enum", "export", "extends", "finally", "for", "function", "if",
    "implements", "import", "in", "instanceof", "interface", "let",
    "new", "of", "private", "protected", "public", "return", "static",
    "super", "switch", "throw", "try", "type", "typeof", "var", "void",
    "while", "yield", NULL };
static const char *js_kw2[] = { "true", "false", "null", "undefined",
    "this", NULL };

static const char *go_kw[] = { "break", "case", "chan", "const",
    "continue", "default", "defer", "else", "fallthrough", "for",
    "func", "go", "goto", "if", "import", "interface", "map", "package",
    "range", "return", "select", "struct", "switch", "type", "var", NULL };
static const char *go_kw2[] = { "bool", "byte", "error", "float32",
    "float64", "int", "int32", "int64", "rune", "string", "uint",
    "uint32", "uint64", "nil", "true", "false", "iota", NULL };

static const char *rs_kw[] = { "as", "async", "await", "break", "const",
    "continue", "crate", "dyn", "else", "enum", "extern", "fn", "for",
    "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut",
    "pub", "ref", "return", "static", "struct", "super", "trait",
    "type", "unsafe", "use", "where", "while", NULL };
static const char *rs_kw2[] = { "bool", "char", "f32", "f64", "i8", "i16",
    "i32", "i64", "isize", "str", "u8", "u16", "u32", "u64", "usize",
    "self", "Self", "String", "Vec", "Option", "Result", "Some", "None",
    "Ok", "Err", "Box", "true", "false", NULL };

static const char *rb_kw[] = { "alias", "and", "begin", "break", "case",
    "class", "def", "do", "else", "elsif", "end", "ensure", "for",
    "if", "in", "module", "next", "not", "or", "raise", "redo",
    "rescue", "retry", "return", "then", "undef", "unless", "until",
    "when", "while", "yield", NULL };
static const char *rb_kw2[] = { "nil", "true", "false", "self", "super",
    "require", "require_relative", "attr_accessor", "attr_reader",
    "attr_writer", "puts", "new", NULL };

static const char *ex_kw[] = { "def", "defp", "defmodule", "defmacro",
    "defmacrop", "defstruct", "defimpl", "defprotocol", "do", "end",
    "fn", "if", "else", "unless", "case", "cond", "with", "for",
    "receive", "after", "rescue", "try", "catch", "raise", "throw",
    "import", "require", "alias", "use", "quote", "unquote", "when",
    "and", "or", "not", "in", NULL };
static const char *ex_kw2[] = { "nil", "true", "false", "__MODULE__",
    "self", NULL };

static const char *json_kw2[] = { "true", "false", "null", NULL };

static const spec specs[] = {
    { .name = "c", .aliases = "c h", .kind = SP_GENERIC,
      .kw = c_kw, .kw2 = c_kw2, .lcom = "//", .bopen = "/*",
      .bclose = "*/", .quotes = "\"'", .hash_preproc = true },
    { .name = "sh", .aliases = "sh psh bash zsh shell pshrc bashrc "
      "zshrc profile", .kind = SP_GENERIC,
      .kw = sh_kw, .lcom = "#", .lcom_boundary = true, .quotes = "\"'",
      .dollar_var = true },
    { .name = "python", .aliases = "py python pyw", .kind = SP_GENERIC,
      .kw = py_kw, .kw2 = py_kw2, .lcom = "#", .quotes = "\"'",
      .triple = true, .at_word = true },
    { .name = "markdown", .aliases = "md markdown mdown",
      .kind = SP_MARKDOWN },
    { .name = "json", .aliases = "json", .kind = SP_GENERIC,
      .kw2 = json_kw2, .lcom = "", .quotes = "\"", .json_keys = true },
    { .name = "diff", .aliases = "diff patch", .kind = SP_DIFF },
    { .name = "yaml", .aliases = "yml yaml", .kind = SP_YAML },
    { .name = "toml", .aliases = "toml", .kind = SP_TOML },
    { .name = "javascript", .aliases = "js jsx mjs cjs ts tsx "
      "javascript typescript", .kind = SP_GENERIC,
      .kw = js_kw, .kw2 = js_kw2, .lcom = "//", .bopen = "/*",
      .bclose = "*/", .quotes = "\"'`", .backtick_multi = true },
    { .name = "go", .aliases = "go golang", .kind = SP_GENERIC,
      .kw = go_kw, .kw2 = go_kw2, .lcom = "//", .bopen = "/*",
      .bclose = "*/", .quotes = "\"'`", .backtick_multi = true },
    /* rust: no ' in quotes — lifetimes ('a) would read as unclosed
     * chars and yellow out the rest of the line */
    { .name = "rust", .aliases = "rs rust", .kind = SP_GENERIC,
      .kw = rs_kw, .kw2 = rs_kw2, .lcom = "//", .bopen = "/*",
      .bclose = "*/", .quotes = "\"", .at_word = false },
    { .name = "ruby", .aliases = "rb ruby rakefile gemfile gemspec",
      .kind = SP_GENERIC,
      .kw = rb_kw, .kw2 = rb_kw2, .lcom = "#", .quotes = "\"'",
      .at_word = true, .colon_sym = true },
    { .name = "elixir", .aliases = "ex exs elixir", .kind = SP_GENERIC,
      .kw = ex_kw, .kw2 = ex_kw2, .lcom = "#", .quotes = "\"'",
      .triple = true, .at_word = true, .colon_sym = true },
};
enum { NSPECS = sizeof specs / sizeof specs[0] };

/* Match a token ("py", "Makefile"…) against a spec's name/aliases. */
static const spec *spec_by_token(const char *tok)
{
    if (!tok || !*tok)
        return NULL;
    for (size_t i = 0; i < NSPECS; i++) {
        if (strcasecmp(tok, specs[i].name) == 0)
            return &specs[i];
        const char *a = specs[i].aliases;
        size_t tl = strlen(tok);
        while (a && *a) {
            while (*a == ' ')
                a++;
            const char *e = a;
            while (*e && *e != ' ')
                e++;
            if ((size_t)(e - a) == tl && strncasecmp(a, tok, tl) == 0)
                return &specs[i];
            a = e;
        }
    }
    return NULL;
}

static const spec *spec_by_filename(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (*base == '.')
        base++; /* .pshrc and friends */
    const spec *s = spec_by_token(base); /* Gemfile, Rakefile, pshrc */
    if (s)
        return s;
    const char *dot = strrchr(base, '.');
    return dot ? spec_by_token(dot + 1) : NULL;
}

static const spec *spec_by_shebang(const char *line)
{
    if (strncmp(line, "#!", 2) != 0)
        return NULL;
    static const struct { const char *needle, *lang; } map[] = {
        { "python", "python" }, { "ruby", "ruby" },
        { "elixir", "elixir" }, { "escript", "elixir" },
        { "node", "javascript" }, { "deno", "javascript" },
        { "bun", "javascript" }, { "psh", "sh" }, { "bash", "sh" },
        { "zsh", "sh" }, { "sh", "sh" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strstr(line, map[i].needle))
            return spec_by_token(map[i].lang);
    return NULL;
}

/* ---------------- the generic engine ---------------- */

/* State that legitimately spans lines. Everything else resets at
 * end-of-line on purpose — see the streaming note up top. */
typedef struct {
    bool in_block; /* inside bopen…bclose */
    char strq;     /* multiline string quote in force, or 0 */
    bool striple;  /* …and it was a triple quote */
} lstate;

static bool ident_start(int c)
{
    return isalpha(c) || c == '_';
}
static bool ident_char(int c)
{
    return isalnum(c) || c == '_';
}

static bool word_in(const char *w, size_t n, const char **set)
{
    if (!set)
        return false;
    for (size_t i = 0; set[i]; i++)
        if (strlen(set[i]) == n && strncmp(set[i], w, n) == 0)
            return true;
    return false;
}

/* Print s[a..b) in the given color. */
static void span(const char *s, size_t a, size_t b, const char *code)
{
    if (a >= b)
        return;
    cput(code);
    fwrite(s + a, 1, b - a, stdout);
    cput(A_RESET);
}

/* Find the end of a string that started at i (opening quote(s)
 * already consumed). Returns the index just past the close and sets
 * *closed; an unterminated string runs to n with *closed false.
 * Honors backslash escapes. */
static size_t string_end(const char *s, size_t n, size_t i, char q,
                         bool triple, bool *closed)
{
    *closed = false;
    while (i < n) {
        if (s[i] == '\\' && i + 1 < n) {
            i += 2;
            continue;
        }
        if (s[i] == q) {
            if (!triple) {
                *closed = true;
                return i + 1;
            }
            if (i + 2 < n && s[i + 1] == q && s[i + 2] == q) {
                *closed = true;
                return i + 3;
            }
        }
        i++;
    }
    return n;
}

static void generic_line(const spec *sp, lstate *st, const char *s)
{
    size_t n = strlen(s);
    size_t i = 0;

    if (st->in_block) {
        const char *e = strstr(s, sp->bclose);
        if (!e) {
            span(s, 0, n, A_COM);
            return;
        }
        size_t end = (size_t)(e - s) + strlen(sp->bclose);
        span(s, 0, end, A_COM);
        st->in_block = false;
        i = end;
    } else if (st->strq) {
        bool closed;
        size_t end = string_end(s, n, 0, st->strq, st->striple, &closed);
        span(s, 0, end, A_STR);
        if (!closed)
            return; /* the string sails on to the next line */
        st->strq = 0;
        i = end;
    }

    /* C-style preprocessor: a leading # owns its directive word. */
    if (sp->hash_preproc && i == 0) {
        size_t j = 0;
        while (j < n && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (j < n && s[j] == '#') {
            size_t k = j + 1;
            while (k < n && ident_char((unsigned char)s[k]))
                k++;
            span(s, 0, k, A_META);
            i = k;
        }
    }

    size_t lcl = sp->lcom ? strlen(sp->lcom) : 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        unsigned char prev = i ? (unsigned char)s[i - 1] : ' ';

        /* line comment — to end of line, we're done */
        if (lcl && strncmp(s + i, sp->lcom, lcl) == 0 &&
            (!sp->lcom_boundary || i == 0 || isspace(prev) ||
             strchr(";|&(", prev))) {
            span(s, i, n, A_COM);
            return;
        }

        /* block comment */
        if (sp->bopen && strncmp(s + i, sp->bopen, strlen(sp->bopen)) == 0) {
            const char *e = strstr(s + i + strlen(sp->bopen), sp->bclose);
            if (!e) {
                span(s, i, n, A_COM);
                st->in_block = true;
                return;
            }
            size_t end = (size_t)(e - s) + strlen(sp->bclose);
            span(s, i, end, A_COM);
            i = end;
            continue;
        }

        /* strings */
        if (sp->quotes && strchr(sp->quotes, c)) {
            bool triple = sp->triple && i + 2 < n && s[i + 1] == (char)c &&
                          s[i + 2] == (char)c;
            size_t body = i + (triple ? 3 : 1);
            bool closed;
            size_t end = string_end(s, n, body, (char)c, triple, &closed);
            const char *code = A_STR;
            if (sp->json_keys && closed) {
                size_t k = end;
                while (k < n && (s[k] == ' ' || s[k] == '\t'))
                    k++;
                if (k < n && s[k] == ':')
                    code = A_KEY;
            }
            span(s, i, end, code);
            if (!closed) {
                if (triple || (c == '`' && sp->backtick_multi)) {
                    st->strq = (char)c; /* genuinely multiline */
                    st->striple = triple;
                }
                return; /* either way the line's spent */
            }
            i = end;
            continue;
        }

        /* $variables (sh) */
        if (sp->dollar_var && c == '$' && i + 1 < n) {
            size_t k = i + 1;
            if (s[k] == '{') {
                while (k < n && s[k] != '}')
                    k++;
                if (k < n)
                    k++;
            } else if (ident_char((unsigned char)s[k]) ||
                       strchr("?#@$!*", s[k])) {
                k++;
                while (k < n && ident_char((unsigned char)s[k]))
                    k++;
            }
            span(s, i, k, A_VAR);
            i = k;
            continue;
        }

        /* @word: decorators, ivars, module attrs */
        if (sp->at_word && c == '@' && i + 1 < n &&
            ident_start((unsigned char)s[i + 1])) {
            size_t k = i + 1;
            while (k < n && ident_char((unsigned char)s[k]))
                k++;
            span(s, i, k, A_META);
            i = k;
            continue;
        }

        /* :symbols / :atoms */
        if (sp->colon_sym && c == ':' && i + 1 < n &&
            ident_start((unsigned char)s[i + 1]) && prev != ':' &&
            !ident_char(prev)) {
            size_t k = i + 1;
            while (k < n && ident_char((unsigned char)s[k]))
                k++;
            span(s, i, k, A_META);
            i = k;
            continue;
        }

        /* numbers */
        if (isdigit(c) && !ident_char(prev)) {
            size_t k = i + 1;
            while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '.' ||
                             s[k] == '_'))
                k++;
            span(s, i, k, A_NUM);
            i = k;
            continue;
        }

        /* words */
        if (ident_start(c)) {
            size_t k = i + 1;
            while (k < n && ident_char((unsigned char)s[k]))
                k++;
            if (word_in(s + i, k - i, sp->kw))
                span(s, i, k, A_KW);
            else if (word_in(s + i, k - i, sp->kw2))
                span(s, i, k, A_TYPE);
            else
                fwrite(s + i, 1, k - i, stdout);
            i = k;
            continue;
        }

        putchar(c);
        i++;
    }
}

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
