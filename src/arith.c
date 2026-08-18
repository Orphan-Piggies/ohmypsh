/*
 * arith.c — the $(( ... )) evaluator.
 *
 * A tiny recursive-descent expression parser over long long, with
 * C's precedence for the operators it supports:
 *
 *   ||   &&   == != < <= > >=   + -   * / %   unary - + !   ( )
 *
 * Bare names are variables, looked up through the usual three tiers
 * (no '$' needed — $((N + 1)) reads N directly, like sh). An unset
 * or non-numeric variable counts as 0. Comparison and logic yield
 * 1/0, so `while [ $((i < 10)) = 1 ]` works even before a real
 * `test` builtin exists.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psh.h"

typedef struct {
    const char *p;
    bool err;
} A;

static long long parse_or(A *a);

static void skipws(A *a)
{
    while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n')
        a->p++;
}

static long long parse_primary(A *a)
{
    skipws(a);
    if (*a->p == '(') {
        a->p++;
        long long v = parse_or(a);
        skipws(a);
        if (*a->p == ')')
            a->p++;
        else
            a->err = true;
        return v;
    }
    if (*a->p == '-') {
        a->p++;
        return -parse_primary(a);
    }
    if (*a->p == '+') {
        a->p++;
        return parse_primary(a);
    }
    if (*a->p == '!') {
        a->p++;
        return !parse_primary(a);
    }
    if (isdigit((unsigned char)*a->p)) {
        char *end;
        long long v = strtoll(a->p, &end, 10);
        a->p = end;
        return v;
    }
    if (isalpha((unsigned char)*a->p) || *a->p == '_') {
        const char *start = a->p;
        while (isalnum((unsigned char)*a->p) || *a->p == '_')
            a->p++;
        char name[256];
        size_t n = (size_t)(a->p - start);
        if (n >= sizeof name)
            n = sizeof name - 1;
        memcpy(name, start, n);
        name[n] = '\0';
        const char *val = psh_var_get(name);
        return val ? strtoll(val, NULL, 10) : 0;
    }
    a->err = true;
    return 0;
}

static long long parse_mul(A *a)
{
    long long v = parse_primary(a);
    for (;;) {
        skipws(a);
        char op = *a->p;
        if (op != '*' && op != '/' && op != '%')
            return v;
        a->p++;
        long long r = parse_primary(a);
        if ((op == '/' || op == '%') && r == 0) {
            a->err = true; /* the shell that divides by zero cracks */
            return 0;
        }
        if (op == '*')
            v *= r;
        else if (op == '/')
            v /= r;
        else
            v %= r;
    }
}

static long long parse_add(A *a)
{
    long long v = parse_mul(a);
    for (;;) {
        skipws(a);
        char op = *a->p;
        if (op != '+' && op != '-')
            return v;
        a->p++;
        long long r = parse_mul(a);
        v = (op == '+') ? v + r : v - r;
    }
}

static long long parse_cmp(A *a)
{
    long long v = parse_add(a);
    for (;;) {
        skipws(a);
        const char *p = a->p;
        if (p[0] == '=' && p[1] == '=') { a->p += 2; v = (v == parse_add(a)); }
        else if (p[0] == '!' && p[1] == '=') { a->p += 2; v = (v != parse_add(a)); }
        else if (p[0] == '<' && p[1] == '=') { a->p += 2; v = (v <= parse_add(a)); }
        else if (p[0] == '>' && p[1] == '=') { a->p += 2; v = (v >= parse_add(a)); }
        else if (p[0] == '<') { a->p += 1; v = (v < parse_add(a)); }
        else if (p[0] == '>') { a->p += 1; v = (v > parse_add(a)); }
        else return v;
    }
}

static long long parse_and(A *a)
{
    long long v = parse_cmp(a);
    for (;;) {
        skipws(a);
        if (a->p[0] != '&' || a->p[1] != '&')
            return v;
        a->p += 2;
        long long r = parse_cmp(a);
        v = (v && r);
    }
}

static long long parse_or(A *a)
{
    long long v = parse_and(a);
    for (;;) {
        skipws(a);
        if (a->p[0] != '|' || a->p[1] != '|')
            return v;
        a->p += 2;
        long long r = parse_and(a);
        v = (v || r);
    }
}

long long psh_arith_eval(const char *expr, bool *err)
{
    A a = { .p = expr, .err = false };
    long long v = parse_or(&a);
    skipws(&a);
    if (*a.p != '\0') /* trailing junk: 1 2, 5..., etc. */
        a.err = true;
    *err = a.err;
    return a.err ? 0 : v;
}
