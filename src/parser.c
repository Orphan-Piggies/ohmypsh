/*
 * parser.c — recursive descent: tokens → syntax tree.
 *
 * Milestone-5 grammar:
 *
 *   program  := stmts
 *   stmts    := (stmt sep?)*                 — sep is ';' or newline
 *   stmt     := (if | while | for | funcdef | list) ['&']
 *   list     := pipeline (('&&' | '||') pipeline)*
 *   pipeline := simple ('|' simple)*
 *   simple   := (WORD | redirect)+
 *   if       := 'if' stmts 'then' stmts
 *               ('elif' stmts 'then' stmts)* ['else' stmts] 'fi'
 *   while    := 'while' stmts 'do' stmts 'done'
 *   for      := 'for' NAME 'in' WORD* sep 'do' stmts 'done'
 *   funcdef  := NAME '(' ')' '{' stmts '}'
 *
 * Two sh-isms worth knowing:
 *
 *   - Keywords are only special in COMMAND POSITION. `echo fi` is a
 *     plain word; only a `fi` where a new command could start closes
 *     an if. That's why `{ echo hi }` needs `; }` — without the
 *     semicolon, `}` is just echo's second argument.
 *
 *   - Newlines separate commands like ';' — except right after
 *     | && ||, where they're skipped so commands can span lines.
 *
 * Running out of tokens INSIDE a construct is not an error, it's
 * *incomplete: the REPL shows a continuation prompt and feeds us the
 * buffer again with more lines appended.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psh.h"

/* ---------------- tree building / freeing ---------------- */

static psh_command *cmd_new(void)
{
    psh_command *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->argv = calloc(1, sizeof *c->argv);
    if (!c->argv) {
        free(c);
        return NULL;
    }
    return c;
}

static bool cmd_add_arg(psh_command *c, const char *word)
{
    char **grown = realloc(c->argv, (c->argc + 2) * sizeof *grown);
    if (!grown)
        return false;
    c->argv = grown;
    c->argv[c->argc] = strdup(word);
    if (!c->argv[c->argc])
        return false;
    c->argc++;
    c->argv[c->argc] = NULL;
    return true;
}

static void cmd_free_chain(psh_command *c)
{
    while (c) {
        psh_command *next = c->next;
        for (size_t i = 0; i < c->argc; i++)
            free(c->argv[i]);
        free(c->argv);
        free(c->in_path);
        free(c->out_path);
        free(c->err_path);
        free(c);
        c = next;
    }
}

static void andor_free_chain(psh_andor *a)
{
    while (a) {
        psh_andor *next = a->next;
        cmd_free_chain(a->pipeline);
        free(a);
        a = next;
    }
}

void psh_stmts_free(psh_stmt *s)
{
    while (s) {
        psh_stmt *next = s->next;
        andor_free_chain(s->list);
        psh_stmts_free(s->cond);
        psh_stmts_free(s->body);
        psh_stmts_free(s->else_body);
        free(s->name);
        for (size_t i = 0; i < s->nwords; i++)
            free(s->words[i]);
        free(s->words);
        psh_case_item *it = s->case_items;
        while (it) {
            psh_case_item *itn = it->next;
            for (size_t i = 0; i < it->npatterns; i++)
                free(it->patterns[i]);
            free(it->patterns);
            psh_stmts_free(it->body);
            free(it);
            it = itn;
        }
        free(s);
        s = next;
    }
}

static psh_stmt *stmt_new(psh_stmt_kind kind)
{
    psh_stmt *s = calloc(1, sizeof *s);
    if (s)
        s->kind = kind;
    return s;
}

/* ---------------- the parser state ---------------- */

typedef struct {
    psh_token *cur;
    int line; /* line of the last token seen (for errors at EOF) */
    bool err;
    bool incomplete;
} P;

static void advance(P *p)
{
    if (p->cur) {
        p->line = p->cur->line;
        p->cur = p->cur->next;
    }
}

static bool at_word(P *p, const char *w)
{
    return p->cur && p->cur->type == TOK_WORD &&
           strcmp(p->cur->text, w) == 0;
}

static void skip_newlines(P *p)
{
    while (p->cur && p->cur->type == TOK_NEWLINE)
        advance(p);
}

static void skip_separators(P *p)
{
    while (p->cur &&
           (p->cur->type == TOK_NEWLINE || p->cur->type == TOK_SEMI))
        advance(p);
}

static void fail(P *p, const char *msg)
{
    int line = p->cur ? p->cur->line : p->line;
    if (line > 1) /* multi-line input or a script: say where */
        fprintf(stderr, "psh: syntax error on line %d: %s\n", line, msg);
    else
        fprintf(stderr, "psh: syntax error: %s\n", msg);
    p->err = true;
}

/* Hitting end-of-input mid-construct = "keep typing", anything else
 * mid-construct = a real error. This one helper is the whole
 * difference between a continuation prompt and a red message. */
static void fail_or_more(P *p, const char *msg)
{
    if (!p->cur)
        p->incomplete = true;
    else
        fail(p, msg);
}

static bool is_name(const char *s)
{
    if (!isalpha((unsigned char)s[0]) && s[0] != '_')
        return false;
    for (size_t i = 1; s[i]; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_')
            return false;
    return true;
}

/* Function NAMES are laxer than variable names: hyphens welcome
 * (venv-init, docker-clean), as in bash and zsh. */
static bool is_funcname(const char *s)
{
    if (!isalpha((unsigned char)s[0]) && s[0] != '_')
        return false;
    for (size_t i = 1; s[i]; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-')
            return false;
    return true;
}

static bool word_in(const char *w, const char **set)
{
    for (size_t i = 0; set[i]; i++)
        if (strcmp(w, set[i]) == 0)
            return true;
    return false;
}

/* ---------------- grammar rules ---------------- */

static psh_stmt *parse_stmts(P *p, const char **stops);

static psh_command *parse_simple(P *p)
{
    psh_command *c = cmd_new();
    if (!c) {
        fail(p, "out of memory");
        return NULL;
    }

    for (;;) {
        if (!p->cur)
            break;
        if (p->cur->type == TOK_WORD) {
            if (!cmd_add_arg(c, p->cur->text)) {
                fail(p, "out of memory");
                goto bad;
            }
            advance(p);
        } else if (p->cur->type == TOK_REDIR_IN ||
                   p->cur->type == TOK_REDIR_OUT ||
                   p->cur->type == TOK_REDIR_APPEND ||
                   p->cur->type == TOK_REDIR_ERR) {
            psh_token_type rt = p->cur->type;
            advance(p);
            if (!p->cur || p->cur->type != TOK_WORD) {
                fail_or_more(p, "expected a filename after redirect");
                goto bad;
            }
            char *path = strdup(p->cur->text);
            if (!path) {
                fail(p, "out of memory");
                goto bad;
            }
            switch (rt) {
            case TOK_REDIR_IN:
                free(c->in_path);
                c->in_path = path;
                break;
            case TOK_REDIR_ERR:
                free(c->err_path);
                c->err_path = path;
                break;
            default:
                free(c->out_path);
                c->out_path = path;
                c->append = (rt == TOK_REDIR_APPEND);
                break;
            }
            advance(p);
        } else {
            break;
        }
    }

    if (c->argc == 0 && !c->in_path && !c->out_path && !c->err_path) {
        fail_or_more(p, "expected a command");
        goto bad;
    }
    return c;
bad:
    cmd_free_chain(c);
    return NULL;
}

static psh_command *parse_pipeline(P *p)
{
    psh_command *head = parse_simple(p);
    if (!head)
        return NULL;
    psh_command *tail = head;
    while (p->cur && p->cur->type == TOK_PIPE) {
        advance(p);
        skip_newlines(p); /* `a |` then newline: keep reading */
        psh_command *next = parse_simple(p);
        if (!next) {
            cmd_free_chain(head);
            return NULL;
        }
        tail->next = next;
        tail = next;
    }
    return head;
}

static psh_stmt *parse_list(P *p)
{
    psh_stmt *s = stmt_new(ST_LIST);
    if (!s) {
        fail(p, "out of memory");
        return NULL;
    }
    psh_andor *atail = NULL;

    for (;;) {
        psh_command *pipe = parse_pipeline(p);
        if (!pipe)
            goto bad;
        psh_andor *a = calloc(1, sizeof *a);
        if (!a) {
            cmd_free_chain(pipe);
            fail(p, "out of memory");
            goto bad;
        }
        a->pipeline = pipe;
        a->conn = PSH_CONN_END;
        if (atail)
            atail->next = a;
        else
            s->list = a;
        atail = a;

        if (p->cur && p->cur->type == TOK_AND)
            a->conn = PSH_CONN_AND;
        else if (p->cur && p->cur->type == TOK_OR)
            a->conn = PSH_CONN_OR;
        else
            break;
        advance(p);
        skip_newlines(p); /* `a &&` then newline: continuation */
    }
    return s;
bad:
    psh_stmts_free(s);
    return NULL;
}

static psh_stmt *parse_if_rest(P *p)
{
    /* 'if' (or 'elif') is already consumed. */
    psh_stmt *s = stmt_new(ST_IF);
    if (!s) {
        fail(p, "out of memory");
        return NULL;
    }

    s->cond = parse_stmts(p, (const char *[]){ "then", NULL });
    if (p->err || p->incomplete)
        goto bad;
    if (!s->cond) {
        fail_or_more(p, "empty condition in 'if'");
        goto bad;
    }
    if (!at_word(p, "then")) {
        fail_or_more(p, "expected 'then'");
        goto bad;
    }
    advance(p);

    s->body = parse_stmts(p, (const char *[]){ "elif", "else", "fi", NULL });
    if (p->err || p->incomplete)
        goto bad;
    if (!s->body) {
        fail_or_more(p, "empty body after 'then'");
        goto bad;
    }

    if (at_word(p, "elif")) {
        advance(p);
        s->else_body = parse_if_rest(p); /* consumes the shared 'fi' */
        if (!s->else_body)
            goto bad;
        return s;
    }
    if (at_word(p, "else")) {
        advance(p);
        s->else_body = parse_stmts(p, (const char *[]){ "fi", NULL });
        if (p->err || p->incomplete)
            goto bad;
        if (!s->else_body) {
            fail_or_more(p, "empty body after 'else'");
            goto bad;
        }
    }
    if (!at_word(p, "fi")) {
        fail_or_more(p, "expected 'fi'");
        goto bad;
    }
    advance(p);
    return s;
bad:
    psh_stmts_free(s);
    return NULL;
}

static psh_stmt *parse_while(P *p)
{
    advance(p); /* 'while' */
    psh_stmt *s = stmt_new(ST_WHILE);
    if (!s) {
        fail(p, "out of memory");
        return NULL;
    }
    s->cond = parse_stmts(p, (const char *[]){ "do", NULL });
    if (p->err || p->incomplete)
        goto bad;
    if (!s->cond) {
        fail_or_more(p, "empty condition in 'while'");
        goto bad;
    }
    if (!at_word(p, "do")) {
        fail_or_more(p, "expected 'do'");
        goto bad;
    }
    advance(p);
    s->body = parse_stmts(p, (const char *[]){ "done", NULL });
    if (p->err || p->incomplete)
        goto bad;
    if (!s->body) {
        fail_or_more(p, "empty body in 'while'");
        goto bad;
    }
    if (!at_word(p, "done")) {
        fail_or_more(p, "expected 'done'");
        goto bad;
    }
    advance(p);
    return s;
bad:
    psh_stmts_free(s);
    return NULL;
}

static psh_stmt *parse_for(P *p)
{
    advance(p); /* 'for' */
    psh_stmt *s = stmt_new(ST_FOR);
    if (!s) {
        fail(p, "out of memory");
        return NULL;
    }
    if (!p->cur || p->cur->type != TOK_WORD || !is_name(p->cur->text)) {
        fail_or_more(p, "expected a variable name after 'for'");
        goto bad;
    }
    s->name = strdup(p->cur->text);
    advance(p);
    skip_newlines(p);
    if (!at_word(p, "in")) {
        fail_or_more(p, "expected 'in' after the 'for' variable");
        goto bad;
    }
    advance(p);

    while (p->cur && p->cur->type == TOK_WORD) {
        char **grown =
            realloc(s->words, (s->nwords + 1) * sizeof *s->words);
        if (!grown) {
            fail(p, "out of memory");
            goto bad;
        }
        s->words = grown;
        s->words[s->nwords] = strdup(p->cur->text);
        if (!s->words[s->nwords]) {
            fail(p, "out of memory");
            goto bad;
        }
        s->nwords++;
        advance(p);
    }

    skip_separators(p);
    if (!at_word(p, "do")) {
        fail_or_more(p, "expected 'do'");
        goto bad;
    }
    advance(p);
    s->body = parse_stmts(p, (const char *[]){ "done", NULL });
    if (p->err || p->incomplete)
        goto bad;
    if (!s->body) {
        fail_or_more(p, "empty body in 'for'");
        goto bad;
    }
    if (!at_word(p, "done")) {
        fail_or_more(p, "expected 'done'");
        goto bad;
    }
    advance(p);
    return s;
bad:
    psh_stmts_free(s);
    return NULL;
}

static psh_stmt *parse_case(P *p)
{
    advance(p); /* 'case' */
    psh_stmt *s = stmt_new(ST_CASE);
    if (!s) {
        fail(p, "out of memory");
        return NULL;
    }
    if (!p->cur || p->cur->type != TOK_WORD) {
        fail_or_more(p, "expected a word after 'case'");
        goto bad;
    }
    s->name = strdup(p->cur->text); /* the raw subject */
    advance(p);
    skip_newlines(p);
    if (!at_word(p, "in")) {
        fail_or_more(p, "expected 'in' after the 'case' subject");
        goto bad;
    }
    advance(p);

    psh_case_item *tail = NULL;
    for (;;) {
        skip_separators(p);
        if (!p->cur) {
            p->incomplete = true;
            goto bad;
        }
        if (at_word(p, "esac")) {
            advance(p);
            return s;
        }

        /* one item:  [ '(' ] pattern ('|' pattern)* ')' stmts [';;'] */
        psh_case_item *item = calloc(1, sizeof *item);
        if (!item) {
            fail(p, "out of memory");
            goto bad;
        }
        if (tail)
            tail->next = item;
        else
            s->case_items = item;
        tail = item;

        if (p->cur->type == TOK_LPAREN)
            advance(p); /* optional opening paren, as in sh */
        for (;;) {
            if (!p->cur || p->cur->type != TOK_WORD) {
                fail_or_more(p, "expected a pattern in 'case'");
                goto bad;
            }
            char **grown = realloc(item->patterns,
                                   (item->npatterns + 1) *
                                       sizeof *item->patterns);
            if (!grown) {
                fail(p, "out of memory");
                goto bad;
            }
            item->patterns = grown;
            item->patterns[item->npatterns] = strdup(p->cur->text);
            item->npatterns++;
            advance(p);
            if (p->cur && p->cur->type == TOK_PIPE) {
                advance(p); /* a|b) — alternation */
                continue;
            }
            break;
        }
        if (!p->cur || p->cur->type != TOK_RPAREN) {
            fail_or_more(p, "expected ')' after the case pattern");
            goto bad;
        }
        advance(p);

        item->body = parse_stmts(p, (const char *[]){ "esac", NULL });
        if (p->err || p->incomplete)
            goto bad;
        /* body may be NULL: an empty item is legal */
        if (p->cur && p->cur->type == TOK_DSEMI)
            advance(p); /* the last item before esac may omit ;; */
    }

bad:
    psh_stmts_free(s);
    return NULL;
}

static psh_stmt *parse_funcdef(P *p)
{
    psh_stmt *s = stmt_new(ST_FUNCDEF);
    if (!s) {
        fail(p, "out of memory");
        return NULL;
    }
    s->name = strdup(p->cur->text);
    advance(p); /* name */
    advance(p); /* ( */
    if (!p->cur || p->cur->type != TOK_RPAREN) {
        fail_or_more(p, "expected ')' in function definition");
        goto bad;
    }
    advance(p);
    skip_newlines(p);
    if (!at_word(p, "{")) {
        fail_or_more(p, "expected '{' to open the function body");
        goto bad;
    }
    advance(p);
    s->body = parse_stmts(p, (const char *[]){ "}", NULL });
    if (p->err || p->incomplete)
        goto bad;
    if (!s->body) {
        fail_or_more(p, "empty function body");
        goto bad;
    }
    if (!at_word(p, "}")) {
        fail_or_more(p, "expected '}' to close the function body");
        goto bad;
    }
    advance(p);
    return s;
bad:
    psh_stmts_free(s);
    return NULL;
}

static psh_stmt *parse_stmt(P *p)
{
    if (at_word(p, "if")) {
        advance(p);
        return parse_if_rest(p);
    }
    if (at_word(p, "while"))
        return parse_while(p);
    if (at_word(p, "for"))
        return parse_for(p);
    if (at_word(p, "case"))
        return parse_case(p);
    if (p->cur && p->cur->type == TOK_WORD && is_funcname(p->cur->text) &&
        p->cur->next && p->cur->next->type == TOK_LPAREN)
        return parse_funcdef(p);
    return parse_list(p);
}

static psh_stmt *parse_stmts(P *p, const char **stops)
{
    psh_stmt *head = NULL, *tail = NULL;
    for (;;) {
        skip_separators(p);
        if (!p->cur)
            break;
        if (p->cur->type == TOK_DSEMI)
            break; /* ';;' ends a case item; the case parser eats it */
        if (stops && p->cur->type == TOK_WORD &&
            word_in(p->cur->text, stops))
            break;

        psh_stmt *s = parse_stmt(p);
        if (!s) {
            psh_stmts_free(head);
            return NULL;
        }
        if (tail)
            tail->next = s;
        else
            head = s;
        tail = s;

        if (p->cur && p->cur->type == TOK_AMP) {
            s->background = true;
            advance(p);
        }
        /* After a statement only a separator, a stop word, or the end
         * may follow; anything else (a stray ')' say) is an error. */
        if (p->cur && p->cur->type != TOK_SEMI &&
            p->cur->type != TOK_NEWLINE && p->cur->type != TOK_DSEMI &&
            !(stops && p->cur->type == TOK_WORD &&
              word_in(p->cur->text, stops))) {
            fail(p, "unexpected token");
            psh_stmts_free(head);
            return NULL;
        }
    }
    return head;
}

psh_stmt *psh_parse(psh_token *tokens, bool *err, bool *incomplete)
{
    P p = { .cur = tokens, .line = 1, .err = false, .incomplete = false };
    psh_stmt *tree = parse_stmts(&p, NULL);
    if (!p.err && !p.incomplete && p.cur) {
        /* parse_stmts stopped on something it couldn't place. */
        fail(&p, "unexpected token");
        psh_stmts_free(tree);
        tree = NULL;
    }
    *err = p.err;
    *incomplete = p.incomplete;
    if (p.err || p.incomplete) {
        psh_stmts_free(tree);
        return NULL;
    }
    return tree;
}
