/*
 * vars.c — the three-tier variable model.
 *
 * Until v0.6, every psh variable was an environment variable: `X=5`
 * silently exported itself to every child process forever. Real
 * shells keep three tiers, and now so does psh:
 *
 *   locals   — declared with `local` inside a function; innermost
 *              scope wins (dynamic scoping); vanish on return
 *   shell    — plain `X=5`; visible to expansion, INVISIBLE to
 *              children
 *   environ  — what fork+exec children inherit; a shell var joins it
 *              via `export`
 *
 * Lookup order is locals → shell → environ. Two deliberate rules:
 *
 *   - Assigning to a name inherited from the environment (PATH=...)
 *     keeps it exported — nobody wants an invisible PATH.
 *   - A `local` shadowing an environment-visible name is mirrored
 *     into the environ for the function's duration and the old value
 *     restored on return — so `local PATH=...` affects the commands
 *     the function runs, like bash.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psh.h"

typedef struct shellvar {
    char *name;
    char *value; /* NULL = export attribute set but no value yet */
    bool exported;
    struct shellvar *next;
} shellvar;

typedef struct localvar {
    char *name;
    char *value;
    bool sync_env;   /* mirrored into environ while in scope */
    char *saved_env; /* environ value to restore on pop; NULL = unset */
    struct localvar *next;
} localvar;

typedef struct scope {
    localvar *vars;
    struct scope *next;
} scope;

static shellvar *shellvars;
static scope *scopes;

static localvar *find_local(const char *name)
{
    for (scope *s = scopes; s; s = s->next)
        for (localvar *v = s->vars; v; v = v->next)
            if (strcmp(v->name, name) == 0)
                return v;
    return NULL;
}

static shellvar *find_shell(const char *name)
{
    for (shellvar *v = shellvars; v; v = v->next)
        if (strcmp(v->name, name) == 0)
            return v;
    return NULL;
}

const char *psh_var_get(const char *name)
{
    localvar *l = find_local(name);
    if (l)
        return l->value;
    shellvar *v = find_shell(name);
    if (v)
        return v->value;
    return getenv(name);
}

void psh_var_set(const char *name, const char *value)
{
    localvar *l = find_local(name);
    if (l) { /* assignment inside a function hits the local first */
        free(l->value);
        l->value = strdup(value);
        if (l->sync_env)
            setenv(name, value, 1);
        return;
    }
    shellvar *v = find_shell(name);
    if (!v) {
        v = calloc(1, sizeof *v);
        if (!v)
            return;
        v->name = strdup(name);
        /* Inherited environment names (PATH, HOME...) stay exported
         * when assigned; brand-new names start shell-only. */
        v->exported = (getenv(name) != NULL);
        v->next = shellvars;
        shellvars = v;
    }
    free(v->value);
    v->value = strdup(value);
    if (v->exported)
        setenv(name, value, 1);
}

void psh_var_export(const char *name)
{
    localvar *l = find_local(name);
    if (l) {
        if (!l->sync_env) {
            const char *env = getenv(name);
            l->saved_env = env ? strdup(env) : NULL;
            l->sync_env = true;
        }
        setenv(name, l->value ? l->value : "", 1);
        return;
    }
    shellvar *v = find_shell(name);
    if (!v) {
        if (getenv(name))
            return; /* inherited env var: already exported */
        v = calloc(1, sizeof *v);
        if (!v)
            return;
        v->name = strdup(name);
        v->value = NULL; /* attribute only; a later X=... fills it */
        v->next = shellvars;
        shellvars = v;
    }
    v->exported = true;
    if (v->value)
        setenv(name, v->value, 1);
}

void psh_var_unset(const char *name)
{
    /* An innermost local: end it early and restore the environ now
     * (its scope's pop will no longer know about it — consistent). */
    for (scope *s = scopes; s; s = s->next) {
        for (localvar **pv = &s->vars; *pv; pv = &(*pv)->next) {
            if (strcmp((*pv)->name, name) == 0) {
                localvar *v = *pv;
                *pv = v->next;
                if (v->sync_env) {
                    if (v->saved_env)
                        setenv(name, v->saved_env, 1);
                    else
                        unsetenv(name);
                }
                free(v->name);
                free(v->value);
                free(v->saved_env);
                free(v);
                return;
            }
        }
    }
    for (shellvar **pv = &shellvars; *pv; pv = &(*pv)->next) {
        if (strcmp((*pv)->name, name) == 0) {
            shellvar *v = *pv;
            *pv = v->next;
            free(v->name);
            free(v->value);
            free(v);
            break;
        }
    }
    unsetenv(name);
}

bool psh_vars_in_function(void)
{
    return scopes != NULL;
}

void psh_vars_push_scope(void)
{
    scope *s = calloc(1, sizeof *s);
    if (!s)
        return;
    s->next = scopes;
    scopes = s;
}

void psh_vars_pop_scope(void)
{
    if (!scopes)
        return;
    scope *s = scopes;
    scopes = s->next;
    localvar *v = s->vars;
    while (v) {
        localvar *next = v->next;
        if (v->sync_env) {
            if (v->saved_env)
                setenv(v->name, v->saved_env, 1);
            else
                unsetenv(v->name);
        }
        free(v->name);
        free(v->value);
        free(v->saved_env);
        free(v);
        v = next;
    }
    free(s);
}

void psh_var_make_local(const char *name, const char *value)
{
    if (!scopes)
        return;
    /* `local X` again in the SAME scope is just assignment. */
    for (localvar *v = scopes->vars; v; v = v->next) {
        if (strcmp(v->name, name) == 0) {
            free(v->value);
            v->value = strdup(value ? value : "");
            if (v->sync_env)
                setenv(name, v->value, 1);
            return;
        }
    }
    localvar *v = calloc(1, sizeof *v);
    if (!v)
        return;
    v->name = strdup(name);
    v->value = strdup(value ? value : "");
    const char *env = getenv(name);
    v->sync_env = (env != NULL);
    v->saved_env = env ? strdup(env) : NULL;
    if (v->sync_env)
        setenv(name, v->value, 1);
    v->next = scopes->vars;
    scopes->vars = v;
}
