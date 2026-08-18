/*
 * complete.c — completion candidates, and the readline glue.
 *
 * Since H4.2 this file has two layers. The ENGINE (readline-free)
 * produces candidate lists and is shared by both line editors:
 *
 *   psh_complete_commands  builtins + $PATH names matching a prefix
 *   psh_complete_files     directory entries matching a path prefix
 *                          (dirs get a trailing '/'; understands ~/)
 *
 * The GLUE at the bottom adapts the command engine to readline's
 * generator protocol; readline's own filename completion still serves
 * as its fallback. The glue goes overboard with readline in H4.4.
 *
 * Candidates don't stat() for the executable bit — bash doesn't
 * either by default, and directory scans should stay cheap.
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <readline/readline.h>

#include "psh.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static char **listv_seal(char **v, size_t n)
{
    if (n == 0) {
        free(v);
        return NULL;
    }
    qsort(v, n, sizeof *v, cmp_str);
    v[n] = NULL;
    return v;
}

void psh_complete_free(char **v)
{
    if (!v)
        return;
    for (size_t i = 0; v[i]; i++)
        free(v[i]);
    free(v);
}

char **psh_complete_commands(const char *prefix)
{
    size_t cap = 32, n = 0;
    char **v = malloc(cap * sizeof *v);
    if (!v)
        return NULL;
    size_t plen = strlen(prefix);

    for (size_t i = 0; psh_builtin_name(i); i++)
        if (strncmp(psh_builtin_name(i), prefix, plen) == 0)
            listv_add(&v, &n, &cap, psh_builtin_name(i));

    const char *path = psh_var_get("PATH");
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
    return listv_seal(v, n);
}

/*
 * Complete `word` as a path. Candidates are BASENAMES (directories
 * get a trailing '/'); *base_off tells the caller where the basename
 * begins inside `word`, i.e. how much of it the candidates replace.
 * A leading ~/ is expanded only for the directory scan — the caller
 * keeps whatever the user actually typed.
 */
char **psh_complete_files(const char *word, size_t *base_off)
{
    const char *slash = strrchr(word, '/');
    size_t doff = slash ? (size_t)(slash - word) + 1 : 0;
    *base_off = doff;
    const char *base = word + doff;
    size_t blen = strlen(base);

    char scan[PATH_MAX];
    if (doff == 0) {
        strcpy(scan, ".");
    } else if (word[0] == '~' && word[1] == '/') {
        const char *home = psh_var_get("HOME");
        snprintf(scan, sizeof scan, "%s%.*s", home ? home : "",
                 (int)(doff - 1), word + 1);
    } else {
        snprintf(scan, sizeof scan, "%.*s", (int)doff, word);
    }

    DIR *d = opendir(scan);
    if (!d)
        return NULL;
    size_t cap = 32, n = 0;
    char **v = malloc(cap * sizeof *v);
    if (!v) {
        closedir(d);
        return NULL;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (e->d_name[0] == '.' && base[0] != '.')
            continue; /* hidden files only when asked for */
        if (strncmp(e->d_name, base, blen) != 0)
            continue;
        char full[PATH_MAX + 300];
        snprintf(full, sizeof full, "%s/%s", scan, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            char with[300];
            snprintf(with, sizeof with, "%s/", e->d_name);
            listv_add(&v, &n, &cap, with);
        } else {
            listv_add(&v, &n, &cap, e->d_name);
        }
    }
    closedir(d);
    return listv_seal(v, n);
}

/* ---------------- readline glue (retires in H4.4) ---------------- */

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
        list = psh_complete_commands(text);
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
