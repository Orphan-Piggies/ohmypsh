/*
 * parser.c — token stream → syntax tree.
 *
 * Milestone-3 grammar (one new layer since M2 — the &&/|| list sits
 * BETWEEN statements and pipelines, which is exactly sh's precedence:
 * `a | b && c` means "(a | b) && c", never "a | (b && c)"):
 *
 *     line     := list (';' list)*
 *     list     := pipeline (('&&' | '||') pipeline)*
 *     pipeline := command ('|' command)*
 *     command  := (WORD | redirect)+
 *     redirect := ('<' | '>' | '>>' | '2>') WORD
 *
 * Redirects may appear anywhere within a command — `>out ls -l` and
 * `ls -l >out` are the same command. A command may even be ONLY
 * redirects (`> file` truncates the file and runs nothing).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psh.h"

static psh_command *cmd_new(void)
{
    psh_command *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->argv = calloc(1, sizeof *c->argv); /* {NULL}: valid empty argv */
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
        free(s);
        s = next;
    }
}

/*
 * The parser is a little assembly line with a partially-built piece
 * at every level: `cur` (command) feeds `phead` (pipeline) feeds
 * `ahead` (&&/|| list) feeds `shead` (statement list). Operators and
 * ';' push pieces down a level; end-of-line flushes everything.
 */
typedef struct {
    psh_stmt *shead, *stail;
    psh_andor *ahead, *atail;
    psh_command *phead, *ptail;
    psh_command *cur;
} builder;

static void push_cur(builder *w)
{
    if (!w->cur)
        return;
    if (w->ptail)
        w->ptail->next = w->cur;
    else
        w->phead = w->cur;
    w->ptail = w->cur;
    w->cur = NULL;
}

/* Close the current pipeline into an &&/|| node joined by `conn`. */
static bool push_pipeline(builder *w, psh_conn conn)
{
    push_cur(w);
    if (!w->phead)
        return true;
    psh_andor *a = calloc(1, sizeof *a);
    if (!a)
        return false;
    a->pipeline = w->phead;
    a->conn = conn;
    if (w->atail)
        w->atail->next = a;
    else
        w->ahead = a;
    w->atail = a;
    w->phead = w->ptail = NULL;
    return true;
}

static bool push_stmt(builder *w)
{
    if (!push_pipeline(w, PSH_CONN_END))
        return false;
    if (!w->ahead)
        return true;
    psh_stmt *s = calloc(1, sizeof *s);
    if (!s)
        return false;
    s->list = w->ahead;
    if (w->stail)
        w->stail->next = s;
    else
        w->shead = s;
    w->stail = s;
    w->ahead = w->atail = NULL;
    return true;
}

psh_stmt *psh_parse(psh_token *tokens, bool *err)
{
    *err = false;
    builder w = { 0 };
    const char *pending = NULL; /* operator that still needs a command */
    const char *msg = NULL;

    for (psh_token *t = tokens; t; t = t->next) {
        switch (t->type) {

        case TOK_WORD:
            if (!w.cur && !(w.cur = cmd_new()))
                goto oom;
            if (!cmd_add_arg(w.cur, t->text))
                goto oom;
            pending = NULL;
            break;

        case TOK_REDIR_IN:
        case TOK_REDIR_OUT:
        case TOK_REDIR_APPEND:
        case TOK_REDIR_ERR: {
            if (!t->next || t->next->type != TOK_WORD) {
                msg = "expected a filename after redirect";
                goto fail;
            }
            if (!w.cur && !(w.cur = cmd_new()))
                goto oom;
            char *path = strdup(t->next->text);
            if (!path)
                goto oom;
            switch (t->type) {
            case TOK_REDIR_IN:
                free(w.cur->in_path);
                w.cur->in_path = path;
                break;
            case TOK_REDIR_ERR:
                free(w.cur->err_path);
                w.cur->err_path = path;
                break;
            default: /* > or >> */
                free(w.cur->out_path);
                w.cur->out_path = path;
                w.cur->append = (t->type == TOK_REDIR_APPEND);
                break;
            }
            t = t->next; /* the filename token is consumed too */
            pending = NULL;
            break;
        }

        case TOK_PIPE:
            if (!w.cur || w.cur->argc == 0) {
                msg = "missing command before '|'";
                goto fail;
            }
            push_cur(&w);
            pending = "|";
            break;

        case TOK_AND:
        case TOK_OR:
            if (!w.cur || w.cur->argc == 0) {
                msg = (t->type == TOK_AND) ? "missing command before '&&'"
                                           : "missing command before '||'";
                goto fail;
            }
            if (!push_pipeline(&w, t->type == TOK_AND ? PSH_CONN_AND
                                                      : PSH_CONN_OR))
                goto oom;
            pending = (t->type == TOK_AND) ? "&&" : "||";
            break;

        case TOK_SEMI:
            if (pending) {
                msg = "expected a command after operator";
                goto fail;
            }
            if (!push_stmt(&w))
                goto oom;
            break;
        }
    }

    /* End of line closes the last statement, same as a ';' would. */
    if (pending) {
        msg = "expected a command after operator";
        goto fail;
    }
    if (!push_stmt(&w))
        goto oom;
    return w.shead;

oom:
    msg = "out of memory";
fail:
    if (pending && msg && strcmp(msg, "expected a command after operator") == 0)
        fprintf(stderr, "psh: syntax error: expected a command after '%s'\n",
                pending);
    else
        fprintf(stderr, "psh: syntax error: %s\n", msg);
    cmd_free_chain(w.cur);
    cmd_free_chain(w.phead);
    andor_free_chain(w.ahead);
    psh_stmts_free(w.shead);
    *err = true;
    return NULL;
}
