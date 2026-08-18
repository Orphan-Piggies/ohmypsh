/*
 * parser.c — token stream → syntax tree.
 *
 * Milestone-2 grammar:
 *
 *     line     := pipeline (';' pipeline)*
 *     pipeline := command ('|' command)*
 *     command  := (WORD | redirect)+
 *     redirect := ('<' | '>' | '>>' | '2>') WORD
 *
 * Redirects may appear anywhere within a command — `>out ls -l` and
 * `ls -l >out` are the same command — which is also how sh reads them.
 * A command may even be ONLY redirects (`> file` creates/truncates the
 * file and runs nothing; a surprisingly handy sh-ism we keep).
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

void psh_stmts_free(psh_stmt *s)
{
    while (s) {
        psh_stmt *next = s->next;
        cmd_free_chain(s->pipeline);
        free(s);
        s = next;
    }
}

psh_stmt *psh_parse(psh_token *tokens, bool *err)
{
    *err = false;

    psh_stmt *shead = NULL, *stail = NULL;    /* finished statements */
    psh_command *phead = NULL, *ptail = NULL; /* pipeline being built */
    psh_command *cur = NULL;                  /* command being built */
    bool after_pipe = false; /* a '|' was seen; a command must follow */
    const char *msg = NULL;

    for (psh_token *t = tokens; t; t = t->next) {
        switch (t->type) {

        case TOK_WORD:
            if (!cur && !(cur = cmd_new()))
                goto oom;
            if (!cmd_add_arg(cur, t->text))
                goto oom;
            break;

        case TOK_REDIR_IN:
        case TOK_REDIR_OUT:
        case TOK_REDIR_APPEND:
        case TOK_REDIR_ERR: {
            /* Every redirect operator must be followed by a filename. */
            if (!t->next || t->next->type != TOK_WORD) {
                msg = "expected a filename after redirect";
                goto fail;
            }
            if (!cur && !(cur = cmd_new()))
                goto oom;
            char *path = strdup(t->next->text);
            if (!path)
                goto oom;
            switch (t->type) {
            case TOK_REDIR_IN:
                free(cur->in_path);
                cur->in_path = path;
                break;
            case TOK_REDIR_ERR:
                free(cur->err_path);
                cur->err_path = path;
                break;
            default: /* > or >> */
                free(cur->out_path);
                cur->out_path = path;
                cur->append = (t->type == TOK_REDIR_APPEND);
                break;
            }
            t = t->next; /* the filename token is consumed too */
            break;
        }

        case TOK_PIPE:
            /* `| cmd` or `cmd | | cmd`: nothing on the left to pipe.
             * (Redirect-only commands can't feed a pipe either.) */
            if (!cur || cur->argc == 0) {
                msg = "missing command before '|'";
                goto fail;
            }
            if (ptail)
                ptail->next = cur;
            else
                phead = cur;
            ptail = cur;
            cur = NULL;
            after_pipe = true;
            break;

        case TOK_SEMI:
            if (after_pipe && !cur) {
                msg = "expected a command after '|'";
                goto fail;
            }
            goto close_statement;
        }
        continue;

    close_statement:
        if (cur) {
            if (ptail)
                ptail->next = cur;
            else
                phead = cur;
            cur = NULL;
        }
        if (phead) {
            psh_stmt *s = calloc(1, sizeof *s);
            if (!s)
                goto oom;
            s->pipeline = phead;
            if (stail)
                stail->next = s;
            else
                shead = s;
            stail = s;
            phead = ptail = NULL;
        }
        after_pipe = false;
    }

    /* End of line closes the last statement, same as a ';' would. */
    if (after_pipe && !cur) {
        msg = "expected a command after '|'";
        goto fail;
    }
    if (cur) {
        if (ptail)
            ptail->next = cur;
        else
            phead = cur;
        cur = NULL;
    }
    if (phead) {
        psh_stmt *s = calloc(1, sizeof *s);
        if (!s)
            goto oom;
        s->pipeline = phead;
        if (stail)
            stail->next = s;
        else
            shead = s;
    }
    return shead;

oom:
    msg = "out of memory";
fail:
    fprintf(stderr, "psh: syntax error: %s\n", msg);
    cmd_free_chain(cur);
    cmd_free_chain(phead);
    psh_stmts_free(shead);
    *err = true;
    return NULL;
}
