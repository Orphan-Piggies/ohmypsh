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

/* Find `name` on $PATH; malloc'd full path or NULL. Shared: type and
 * command -v here, and the cockpit's syntax highlighting (editor.c)
 * asks it whether the command you're typing exists. */
char *psh_path_lookup(const char *name)
{
    if (strchr(name, '/'))
        return access(name, X_OK) == 0 ? strdup(name) : NULL;
    const char *path = psh_var_get("PATH");
    if (!path)
        return NULL;
    char *copy = strdup(path);
    if (!copy)
        return NULL;
    char full[PATH_MAX];
    for (char *dir = strtok(copy, ":"); dir; dir = strtok(NULL, ":")) {
        snprintf(full, sizeof full, "%s/%s", dir, name);
        if (access(full, X_OK) == 0) {
            free(copy);
            return strdup(full);
        }
    }
    free(copy);
    return NULL;
}

static int bi_type(char **argv)
{
    int rc = 0;
    for (size_t i = 1; argv[i]; i++) {
        if (psh_function_exists(argv[i])) {
            printf("%s is a function\n", argv[i]);
        } else if (psh_find_builtin(argv[i])) {
            printf("%s is a shell builtin\n", argv[i]);
        } else {
            char *p = psh_path_lookup(argv[i]);
            if (p) {
                printf("%s is %s\n", argv[i], p);
                free(p);
            } else {
                fprintf(stderr, "psh: type: %s: not found\n", argv[i]);
                rc = 1;
            }
        }
    }
    return rc;
}

/* `command -v NAME` — the POSIX way plugins ask "is this installed?".
 * Prints the name (function/builtin) or path (file); silent + status
 * 1 when missing, so `if command -v rails > /dev/null` just works. */
static int bi_command(char **argv)
{
    if (!argv[1] || strcmp(argv[1], "-v") != 0 || !argv[2]) {
        fprintf(stderr,
                "psh: command: only 'command -v NAME' is supported\n");
        return 2;
    }
    int rc = 0;
    for (size_t i = 2; argv[i]; i++) {
        if (psh_function_exists(argv[i]) || psh_find_builtin(argv[i])) {
            puts(argv[i]);
        } else {
            char *p = psh_path_lookup(argv[i]);
            if (p) {
                puts(p);
                free(p);
            } else {
                rc = 1;
            }
        }
    }
    return rc;
}

static int bi_set(char **argv)
{
    for (size_t i = 1; argv[i]; i++) {
        if (strcmp(argv[i], "-e") == 0)
            psh_errexit = true;
        else if (strcmp(argv[i], "+e") == 0)
            psh_errexit = false;
        else {
            fprintf(stderr, "psh: set: %s: unknown option\n", argv[i]);
            return 2;
        }
    }
    return 0;
}

/*
 * trap 'commands' EXIT — run commands when THIS shell exits (any
 * path: `exit`, Ctrl-D, end of script; atexit catches them all).
 * Children die via _exit, which skips atexit — so a fork never fires
 * the parent's trap. The pid check guards the one exception: a
 * builtin `exit` inside a pipeline child calls exit() proper.
 */
static char *trap_exit_cmd;
static pid_t trap_owner;

static void run_exit_trap(void)
{
    static bool running;
    if (trap_exit_cmd && !running && getpid() == trap_owner) {
        running = true;
        psh_run_string(trap_exit_cmd);
    }
}

static int bi_trap(char **argv)
{
    if (!argv[1]) {
        if (trap_exit_cmd)
            printf("trap -- '%s' EXIT\n", trap_exit_cmd);
        return 0;
    }
    if (!argv[2] || strcmp(argv[2], "EXIT") != 0) {
        fprintf(stderr, "psh: trap: only the EXIT trap is supported\n");
        return 2;
    }
    if (strcmp(argv[1], "-") == 0) {
        free(trap_exit_cmd);
        trap_exit_cmd = NULL;
        return 0;
    }
    free(trap_exit_cmd);
    trap_exit_cmd = strdup(argv[1]);
    if (!trap_owner) {
        trap_owner = getpid();
        atexit(run_exit_trap);
    }
    return 0;
}

/*
 * history      — every line the cockpit remembers, numbered like bash
 * history N    — just the last N
 * history -c   — forget it all (the file follows suit at exit)
 *
 * The list lives in editor.c and is only fed interactively, so in a
 * script this prints nothing — which is also the honest answer.
 */
static int bi_history(char **argv)
{
    if (argv[1] && strcmp(argv[1], "-c") == 0) {
        psh_editor_hist_clear();
        return 0;
    }
    size_t n = psh_editor_hist_count(), first = 0;
    if (argv[1]) {
        char *end;
        long want = strtol(argv[1], &end, 10);
        if (*end || end == argv[1] || want < 0) {
            fprintf(stderr, "psh: history: %s: expected a count or -c\n",
                    argv[1]);
            return 2;
        }
        if ((size_t)want < n)
            first = n - (size_t)want;
    }
    for (size_t i = first; i < n; i++)
        printf("%5zu  %s\n", i + 1, psh_editor_hist_get(i));
    return 0;
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
    { "test",     psh_builtin_test, "evaluate an expression (no fork)" },
    { "[",        psh_builtin_test, "test, wearing its bracket" },
    { "type",     bi_type,        "say what a name is (function/builtin/file)" },
    { "command",  bi_command,     "command -v NAME: locate a command" },
    { "set",      bi_set,         "set -e: exit on first failure (+e undoes)" },
    { "trap",     bi_trap,        "trap 'cmds' EXIT: run commands at exit" },
    { "export",   bi_export,      "make a variable visible to children" },
    { "local",    bi_local,       "function-scoped variable" },
    { "unset",    bi_unset,       "remove a variable" },
    { "source",   bi_source,      "run a file in THIS shell (also: .)" },
    { ".",        bi_source,      "alias for source" },
    { "return",   bi_return,      "return from a function (return [n])" },
    { "break",    bi_break,       "exit the enclosing loop" },
    { "continue", bi_continue,    "next iteration of the enclosing loop" },
    { "history",  bi_history,     "the story so far (history [N], -c forgets)" },
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
