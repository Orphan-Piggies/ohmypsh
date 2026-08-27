/*
 * hl.h — the armory's shared highlight engine.
 *
 * Born inside salt (which colors bytes and changes nothing) and
 * extracted so roast (which renders Markdown and changes plenty)
 * can light up fenced code the same way. Header-only on purpose:
 * two small binaries, zero link ceremony.
 *
 * Contents: the palette, the language specs (13 of them), detection
 * by token / filename / shebang, and the line-at-a-time generic
 * engine. Only genuinely multi-line constructs carry state across
 * lines (block comments, triple quotes, template/raw strings) — an
 * unclosed plain string resets at end-of-line, so joining a stream
 * mid-file can miscolor one line, never the session.
 */
#ifndef HL_H
#define HL_H

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

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

static inline void cput(const char *code)
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
static inline const spec *spec_by_token(const char *tok)
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

static inline const spec *spec_by_filename(const char *path)
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

static inline const spec *spec_by_shebang(const char *line)
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

static inline bool ident_start(int c)
{
    return isalpha(c) || c == '_';
}
static inline bool ident_char(int c)
{
    return isalnum(c) || c == '_';
}

static inline bool word_in(const char *w, size_t n, const char **set)
{
    if (!set)
        return false;
    for (size_t i = 0; set[i]; i++)
        if (strlen(set[i]) == n && strncmp(set[i], w, n) == 0)
            return true;
    return false;
}

/* Print s[a..b) in the given color. */
static inline void span(const char *s, size_t a, size_t b, const char *code)
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
static inline size_t string_end(const char *s, size_t n, size_t i, char q,
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

static inline void generic_line(const spec *sp, lstate *st, const char *s)
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

#endif /* HL_H */
