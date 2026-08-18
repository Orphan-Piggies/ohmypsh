/*
 * builtins.c — commands that must run inside the shell process.
 *
 * The test for "does this need to be a builtin?" is: does it change the
 * shell's own state? cd changes our working directory, exit ends our
 * process — no external program could do either on our behalf.
 * (help and crack are builtins out of convenience, not necessity.)
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psh.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int bi_cd(char **argv)
{
    const char *target = argv[1];
    if (!target)
        target = psh_var_get("HOME");       /* bare `cd` goes home */
    else if (strcmp(target, "-") == 0) {
        target = psh_var_get("OLDPWD");     /* `cd -` bounces back */
        if (!target) {
            fprintf(stderr, "psh: cd: OLDPWD not set\n");
            return 1;
        }
        puts(target); /* bash prints where `cd -` took you */
    }
    if (!target) {
        fprintf(stderr, "psh: cd: HOME not set\n");
        return 1;
    }

    char oldpwd[PATH_MAX];
    char *had_old = getcwd(oldpwd, sizeof oldpwd);

    if (chdir(target) != 0) {
        fprintf(stderr, "psh: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    /* Keep OLDPWD/PWD in sync — and exported, so child programs see
     * them too (many tools read $PWD). */
    if (had_old) {
        psh_var_set("OLDPWD", oldpwd);
        psh_var_export("OLDPWD");
    }
    char newpwd[PATH_MAX];
    if (getcwd(newpwd, sizeof newpwd)) {
        psh_var_set("PWD", newpwd);
        psh_var_export("PWD");
    }
    return 0;
}

/* export / local / unset — the visible face of the variable table. */
static int bi_export(char **argv)
{
    for (size_t i = 1; argv[i]; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) { /* export X=v : set, then promote */
            char *name = strndup(argv[i], (size_t)(eq - argv[i]));
            if (name) {
                psh_var_set(name, eq + 1);
                psh_var_export(name);
                free(name);
            }
        } else {
            psh_var_export(argv[i]);
        }
    }
    return 0;
}

static int bi_local(char **argv)
{
    if (!psh_vars_in_function()) {
        fprintf(stderr, "psh: local: only meaningful inside a function\n");
        return 1;
    }
    for (size_t i = 1; argv[i]; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            char *name = strndup(argv[i], (size_t)(eq - argv[i]));
            if (name) {
                psh_var_make_local(name, eq + 1);
                free(name);
            }
        } else {
            psh_var_make_local(argv[i], NULL);
        }
    }
    return 0;
}

static int bi_unset(char **argv)
{
    for (size_t i = 1; argv[i]; i++)
        psh_var_unset(argv[i]);
    return 0;
}

static int bi_exit(char **argv)
{
    /* exit ends the shell itself, so it never returns. */
    exit(argv[1] ? atoi(argv[1]) & 0xff : 0);
}

static int bi_pwd(char **argv)
{
    (void)argv;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, "psh: pwd: %s\n", strerror(errno));
        return 1;
    }
    puts(cwd);
    return 0;
}

/* Control flow: raise a flag; the executors in exec.c consume it.
 * `return` also carries a status (default: the current $?). */
static int bi_return(char **argv)
{
    psh_flow = PSH_FLOW_RETURN;
    return argv[1] ? atoi(argv[1]) & 0xff : psh_last_status;
}

static int bi_break(char **argv)
{
    (void)argv;
    psh_flow = PSH_FLOW_BREAK;
    return 0;
}

static int bi_continue(char **argv)
{
    (void)argv;
    psh_flow = PSH_FLOW_CONTINUE;
    return 0;
}

static int bi_source(char **argv)
{
    if (!argv[1]) {
        fprintf(stderr, "psh: source: filename required\n");
        return 2;
    }
    return psh_run_file(argv[1]);
}

static int bi_help(char **argv)
{
    (void)argv;
    printf("psh %s — the pistachio shell " PSH_NUT "\n", PSH_VERSION);
    printf("builtins:\n");
    psh_list_builtins();
    printf("everything else is found via $PATH and run as a child process.\n");
    return 0;
}

static const struct {
    const char *name;
    psh_builtin_fn fn;
    const char *blurb;
} builtins[] = {
    { "cd",    bi_cd,             "change directory (cd, cd <dir>, cd -)" },
    { "exit",  bi_exit,           "leave the bag (exit [status])" },
    { "pwd",   bi_pwd,            "print working directory" },
    { "jobs",  psh_builtin_jobs,  "list background and stopped jobs" },
    { "fg",    psh_builtin_fg,    "bring a job to the foreground (fg [%n])" },
    { "bg",    psh_builtin_bg,    "continue a stopped job in the background" },
    { "wait",  psh_builtin_wait,  "wait for all background jobs to finish" },
    { "export",   bi_export,      "make a variable visible to children" },
    { "local",    bi_local,       "function-scoped variable" },
    { "unset",    bi_unset,       "remove a variable" },
    { "source",   bi_source,      "run a file in THIS shell (also: .)" },
    { ".",        bi_source,      "alias for source" },
    { "return",   bi_return,      "return from a function (return [n])" },
    { "break",    bi_break,       "exit the enclosing loop" },
    { "continue", bi_continue,    "next iteration of the enclosing loop" },
    { "help",  bi_help,           "this text" },
    { "crack", psh_builtin_crack, "???" },
};

const char *psh_builtin_name(size_t i)
{
    if (i >= sizeof builtins / sizeof builtins[0])
        return NULL;
    return builtins[i].name;
}

psh_builtin_fn psh_find_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        if (strcmp(name, builtins[i].name) == 0)
            return builtins[i].fn;
    return NULL;
}

void psh_list_builtins(void)
{
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        printf("  %-6s %s\n", builtins[i].name, builtins[i].blurb);
}
