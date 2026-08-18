/*
 * complete.c — tab completion.
 *
 * Readline already ships filename completion; the only thing a shell
 * must add is COMMAND completion for the first word of a command.
 * "First word" means: at the start of the line, or right after an
 * operator that begins a new command (| ; & && ||).
 *
 * Candidates are the builtins plus every executable-looking name in
 * $PATH. We don't stat() for the executable bit — bash doesn't
 * either by default, and directory scans should stay cheap.
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <readline/readline.h>

#include "psh.h"

static bool listv_add(char ***v, size_t *n, size_t *cap, const char *s)
{
    for (size_t k = 0; k < *n; k++) /* dedup: PATH dirs overlap */
        if (strcmp((*v)[k], s) == 0)
            return true;
    if (*n + 2 > *cap) {
        size_t newcap = *cap * 2;
        char **grown = realloc(*v, newcap * sizeof **v);
        if (!grown)
            return false;
        *v = grown;
        *cap = newcap;
    }
    (*v)[*n] = strdup(s);
    if (!(*v)[*n])
        return false;
    (*n)++;
    return true;
}

static char **collect_commands(const char *prefix)
{
    size_t cap = 32, n = 0;
    char **v = malloc(cap * sizeof *v);
    if (!v)
        return NULL;
    size_t plen = strlen(prefix);

    for (size_t i = 0; psh_builtin_name(i); i++)
        if (strncmp(psh_builtin_name(i), prefix, plen) == 0)
            listv_add(&v, &n, &cap, psh_builtin_name(i));

    const char *path = getenv("PATH");
    char *copy = path ? strdup(path) : NULL;
    if (copy) {
        for (char *dir = strtok(copy, ":"); dir; dir = strtok(NULL, ":")) {
            DIR *d = opendir(dir);
            if (!d)
                continue;
            struct dirent *e;
            while ((e = readdir(d)) != NULL)
                if (e->d_name[0] != '.' &&
                    strncmp(e->d_name, prefix, plen) == 0)
                    listv_add(&v, &n, &cap, e->d_name);
            closedir(d);
        }
        free(copy);
    }

    if (n == 0) {
        free(v);
        return NULL;
    }
    v[n] = NULL;
    return v;
}

/*
 * Readline's generator protocol: called with state==0 to start, then
 * repeatedly; return one malloc'd match per call (readline frees
 * them), NULL when done.
 */
static char *cmd_generator(const char *text, int state)
{
    static char **list;
    static size_t idx;

    if (state == 0) {
        list = collect_commands(text);
        idx = 0;
    }
    if (list && list[idx])
        return list[idx++]; /* ownership moves to readline */
    free(list);             /* container only; strings were handed out */
    list = NULL;
    return NULL;
}

static char **complete_hook(const char *text, int start, int end)
{
    (void)end;
    /* Command position? Look at the last non-blank before the word:
     * nothing, or an operator, means a command starts here. */
    bool cmdpos = true;
    for (int k = start - 1; k >= 0; k--) {
        char ch = rl_line_buffer[k];
        if (ch == ' ' || ch == '\t')
            continue;
        cmdpos = (ch == '|' || ch == ';' || ch == '&');
        break;
    }
    if (cmdpos)
        return rl_completion_matches(text, cmd_generator);
    return NULL; /* NULL → readline falls back to filename completion */
}

void psh_completion_init(void)
{
    rl_attempted_completion_function = complete_hook;
}
