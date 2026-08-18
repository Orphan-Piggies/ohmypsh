/*
 * testcmd.c — the test / [ builtin.
 *
 * Until now every `if [ ... ]` forked /usr/bin/[ — a whole process
 * to compare two strings. As a builtin, a loop like
 * `while [ $((i < 1000)) = 1 ]` forks nothing at all.
 *
 * Supported grammar (POSIX test, the parts people use):
 *
 *   or    := and (-o and)*
 *   and   := not (-a not)*
 *   not   := '!' not | primary
 *   primary :=
 *       STRING op STRING      =  ==  !=  -eq -ne -lt -le -gt -ge
 *     | -e|-f|-d|-L|-h|-s|-r|-w|-x FILE
 *     | -n STRING | -z STRING
 *     | STRING                (true if non-empty)
 *
 * `[` demands a closing `]`. Exit: 0 true, 1 false, 2 bad expression.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "psh.h"

typedef struct {
    char **argv;
    int i, argc;
    bool err;
} T;

static bool t_or(T *t);

static const char *cur(T *t)
{
    return t->i < t->argc ? t->argv[t->i] : NULL;
}

static bool to_num(const char *s, long long *out)
{
    char *end;
    *out = strtoll(s, &end, 10);
    return *s != '\0' && *end == '\0';
}

static bool is_binop(const char *s)
{
    static const char *ops[] = { "=",   "==",  "!=",  "-eq", "-ne",
                                 "-lt", "-le", "-gt", "-ge", NULL };
    for (size_t k = 0; ops[k]; k++)
        if (strcmp(s, ops[k]) == 0)
            return true;
    return false;
}

static bool eval_binary(T *t, const char *a, const char *op, const char *b)
{
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
        return strcmp(a, b) == 0;
    if (strcmp(op, "!=") == 0)
        return strcmp(a, b) != 0;

    long long x, y;
    if (!to_num(a, &x) || !to_num(b, &y)) {
        fprintf(stderr, "psh: test: %s: integer expected\n",
                to_num(a, &x) ? b : a);
        t->err = true;
        return false;
    }
    if (strcmp(op, "-eq") == 0) return x == y;
    if (strcmp(op, "-ne") == 0) return x != y;
    if (strcmp(op, "-lt") == 0) return x < y;
    if (strcmp(op, "-le") == 0) return x <= y;
    if (strcmp(op, "-gt") == 0) return x > y;
    return x >= y; /* -ge */
}

static bool eval_unary(T *t, char op, const char *arg)
{
    struct stat st;
    switch (op) {
    case 'n': return arg[0] != '\0';
    case 'z': return arg[0] == '\0';
    case 'e': return stat(arg, &st) == 0;
    case 'f': return stat(arg, &st) == 0 && S_ISREG(st.st_mode);
    case 'd': return stat(arg, &st) == 0 && S_ISDIR(st.st_mode);
    case 's': return stat(arg, &st) == 0 && st.st_size > 0;
    case 'L':
    case 'h': return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);
    case 'r': return access(arg, R_OK) == 0;
    case 'w': return access(arg, W_OK) == 0;
    case 'x': return access(arg, X_OK) == 0;
    default:
        fprintf(stderr, "psh: test: -%c: unknown operator\n", op);
        t->err = true;
        return false;
    }
}

static bool t_primary(T *t)
{
    const char *a = cur(t);
    if (!a) {
        t->err = true;
        return false;
    }
    /* Binary has priority: `-n = -n` compares the strings, like sh. */
    if (t->i + 1 < t->argc && is_binop(t->argv[t->i + 1])) {
        if (t->i + 2 >= t->argc) {
            fprintf(stderr, "psh: test: %s: missing right operand\n",
                    t->argv[t->i + 1]);
            t->err = true;
            return false;
        }
        bool v = eval_binary(t, a, t->argv[t->i + 1], t->argv[t->i + 2]);
        t->i += 3;
        return v;
    }
    if (a[0] == '-' && a[1] != '\0' && a[2] == '\0' &&
        t->i + 1 < t->argc) {
        bool v = eval_unary(t, a[1], t->argv[t->i + 1]);
        t->i += 2;
        return v;
    }
    t->i += 1;
    return a[0] != '\0'; /* bare string: non-empty is true */
}

static bool t_not(T *t)
{
    if (cur(t) && strcmp(cur(t), "!") == 0) {
        t->i++;
        return !t_not(t);
    }
    return t_primary(t);
}

static bool t_and(T *t)
{
    bool v = t_not(t);
    while (cur(t) && strcmp(cur(t), "-a") == 0) {
        t->i++;
        bool r = t_not(t);
        v = v && r;
    }
    return v;
}

static bool t_or(T *t)
{
    bool v = t_and(t);
    while (cur(t) && strcmp(cur(t), "-o") == 0) {
        t->i++;
        bool r = t_and(t);
        v = v || r;
    }
    return v;
}

int psh_builtin_test(char **argv)
{
    int argc = 0;
    while (argv[argc])
        argc++;

    if (strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "psh: [: missing ']'\n");
            return 2;
        }
        argc--; /* the ] is punctuation, not an argument */
    }
    if (argc == 1)
        return 1; /* `test` with no arguments: false */

    T t = { .argv = argv, .i = 1, .argc = argc, .err = false };
    bool v = t_or(&t);
    if (t.err || t.i != argc) {
        if (!t.err)
            fprintf(stderr, "psh: %s: bad expression\n", argv[0]);
        return 2;
    }
    return v ? 0 : 1;
}
