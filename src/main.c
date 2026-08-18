/*
 * main.c — the REPL, and every other way input reaches the shell:
 *
 *   psh              interactive: the cockpit (editor.c), history,
 *                    continuation prompts for multi-line constructs
 *   psh file [args]  run a script; args become $1..$9, $#
 *   psh -c 'cmd'     run one string and exit
 *   (piped stdin)    non-interactive line loop
 *
 * The interesting M5 change is ACCUMULATION: lexer/parser can answer
 * "incomplete" instead of "error", meaning the input is fine but
 * unfinished (an open `if`, quote, or `$( `). Interactively that
 * shows a `> ` continuation prompt; at a script's EOF it's an error.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "psh.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int psh_last_status = 0;
volatile sig_atomic_t psh_interrupted = 0;

char **psh_script_args = NULL;
size_t psh_script_argc = 0;
const char *psh_arg0 = "psh";

static char hist_path[PATH_MAX];

static void save_history(void)
{
    if (hist_path[0])
        psh_editor_hist_save(hist_path);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/*
 * Interactive Ctrl-C: readline installs its own handler while it
 * reads (aborting the current input line); OURS is active the rest
 * of the time — i.e. while parent-side loops run — where it sets a
 * flag the executors poll. That's how `while true; do true; done`
 * stays killable. Foreground JOBS never need this: the kernel sends
 * their SIGINT straight to their process group, not to the shell.
 */
static void on_sigint(int sig)
{
    (void)sig;
    psh_interrupted = 1;
}

/* feed() outcomes */
enum { FEED_DONE, FEED_MORE };

static int feed(const char *buf)
{
    bool err = false, incomplete = false;
    psh_token *tokens = psh_tokenize(buf, &err, &incomplete);
    if (incomplete)
        return FEED_MORE;
    if (err) {
        psh_last_status = 2;
        return FEED_DONE;
    }

    psh_stmt *stmts = psh_parse(tokens, &err, &incomplete);
    psh_tokens_free(tokens);
    if (incomplete)
        return FEED_MORE;
    if (err) {
        psh_last_status = 2;
        return FEED_DONE;
    }

    if (stmts) {
        psh_interrupted = 0;
        psh_last_status = psh_execute(stmts);
        psh_stmts_free(stmts);
        /* A top-level `return` stops this unit of input (that's how
         * `return` in a SOURCED file works); it must not leak into
         * the next line. Function returns never reach here — they're
         * absorbed at the function boundary. */
        psh_flow = PSH_FLOW_NONE;
    }
    return FEED_DONE;
}

/* A complete unit of input (command substitution, -c, whole files):
 * "incomplete" here means the input ended mid-construct — an error. */
int psh_run_string(const char *s)
{
    if (feed(s) == FEED_MORE) {
        fprintf(stderr, "psh: syntax error: unexpected end of input\n");
        psh_last_status = 2;
    }
    return psh_last_status;
}

static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (cap - len < 2) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = grown;
        }
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

int psh_run_file(const char *path)
{
    char *buf = read_whole_file(path);
    if (!buf) {
        fprintf(stderr, "psh: %s: %s\n", path, strerror(errno));
        psh_last_status = 127;
        return 127;
    }
    int status = psh_run_string(buf);
    free(buf);
    return status;
}

/* ~/.pshrc: run once at interactive startup, exactly as if typed.
 * The place for greetings, variables, functions — oh-my-psh will
 * live on top of this file. */
static void source_pshrc(void)
{
    const char *home = psh_var_get("HOME");
    if (!home)
        return;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.pshrc", home);
    if (access(path, R_OK) == 0)
        psh_run_file(path);
}

/*
 * Themed prompt: if PSH_PROMPT is set, the prompt is that template
 * EXPANDED FRESH each time — $VAR and $(...) run per prompt, which is
 * the entire mechanism behind oh-my-psh themes ($(git_branch) in your
 * prompt is just command substitution). After expansion, `\e` becomes
 * ESC and `\n` a newline, and every escape sequence is wrapped in
 * \001/\002 so readline can compute the prompt's true visible width.
 */
static bool themed_prompt(char *out, size_t outsz)
{
    const char *tpl = psh_var_get("PSH_PROMPT");
    if (!tpl || !*tpl)
        return false;
    char *x = psh_expand_word_single(tpl);
    if (!x)
        return false;

    size_t o = 0;
    const char *p = x;
    while (*p && o + 8 < outsz) {
        if (*p == '\033' || (p[0] == '\\' && p[1] == 'e')) {
            out[o++] = '\001'; /* "invisible from here..." */
            out[o++] = '\033';
            p += (*p == '\033') ? 1 : 2;
            while (*p && o + 4 < outsz) { /* copy up to the final letter */
                char c = *p++;
                out[o++] = c;
                if (isalpha((unsigned char)c))
                    break;
            }
            out[o++] = '\002'; /* "...to here" */
        } else if (p[0] == '\\' && p[1] == 'n') {
            out[o++] = '\n';
            p += 2;
        } else if (p[0] == '\\' && p[1] == '\\') {
            out[o++] = '\\';
            p += 2;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    free(x);
    return true;
}

/*
 * Default prompt: cwd with $HOME shortened to ~, and the last
 * failure status in red. The \001/\002 bytes bracket color escapes
 * so readline can compute the prompt's true width — without them,
 * line editing garbles after the first ↑.
 */
static void build_prompt(char *out, size_t outsz)
{
    if (themed_prompt(out, outsz))
        return;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "?");

    const char *home = psh_var_get("HOME");
    size_t hlen = home ? strlen(home) : 0;
    const char *disp = cwd;
    char shortened[PATH_MAX + 2];
    if (home && strncmp(cwd, home, hlen) == 0 &&
        (cwd[hlen] == '/' || cwd[hlen] == '\0')) {
        snprintf(shortened, sizeof shortened, "~%s", cwd + hlen);
        disp = shortened;
    }

    if (psh_last_status == 0)
        snprintf(out, outsz,
                 PSH_NUT " \001\033[32m\002%s\001\033[0m\002 $ ", disp);
    else
        snprintf(out, outsz,
                 PSH_NUT " \001\033[32m\002%s\001\033[0m\002 "
                 "\001\033[31m\002[%d]\001\033[0m\002 $ ",
                 disp, psh_last_status);
}

/* Append `line` plus a newline to the accumulation buffer. */
static bool acc_append(char **acc, const char *line)
{
    size_t old = *acc ? strlen(*acc) : 0;
    char *grown = realloc(*acc, old + strlen(line) + 2);
    if (!grown)
        return false;
    if (!old)
        grown[0] = '\0';
    strcat(grown, line);
    strcat(grown, "\n");
    *acc = grown;
    return true;
}

static void repl_interactive(void)
{
    psh_pistachio_hello();

    const char *home = psh_var_get("HOME");
    if (home) {
        snprintf(hist_path, sizeof hist_path, "%s/.psh_history", home);
        psh_editor_hist_load(hist_path);
        /* atexit catches BOTH exit paths: Ctrl-D and the `exit`
         * builtin (which calls exit() from deep in builtins.c). */
        atexit(save_history);
    }

    source_pshrc();

    char prompt[PATH_MAX + 64];
    char *acc = NULL; /* multi-line command being accumulated */
    for (;;) {
        if (!acc) {
            /* The oh-my-psh event hook: plugins define omp_precmd and
             * it runs before every prompt. $? is preserved so the
             * prompt (and the user) still see the real last status. */
            if (psh_function_exists("omp_precmd")) {
                int saved = psh_last_status;
                psh_run_string("omp_precmd");
                psh_last_status = saved;
            }
            psh_jobs_notify();
            build_prompt(prompt, sizeof prompt);
        }
        char *line = psh_editor_readline(acc ? "  > " : prompt);
        if (!line) { /* Ctrl-D */
            if (acc) { /* abandon the half-typed construct */
                fprintf(stderr,
                        "psh: syntax error: unexpected end of input\n");
                free(acc);
                acc = NULL;
                psh_last_status = 2;
                continue;
            }
            puts("exit");
            break;
        }
        if (*line)
            psh_editor_hist_add(line);
        if (!acc_append(&acc, line)) {
            free(line);
            continue;
        }
        free(line);
        /* Time the unit and publish $PSH_CMD_MS — themes and the
         * duration plugin read it. Only for lines that DID something
         * (a bare Enter shouldn't wipe the last reading), and only
         * here: omp_precmd runs through psh_run_string, so hooks
         * never overwrite the measurement they're about to show. */
        bool ran = acc && acc[strspn(acc, " \t\n")] != '\0';
        double t0 = now_ms();
        if (feed(acc) == FEED_DONE) {
            if (ran) {
                char ms[32];
                snprintf(ms, sizeof ms, "%.0f", now_ms() - t0);
                psh_var_set("PSH_CMD_MS", ms);
            }
            free(acc);
            acc = NULL;
        }
    }
    free(acc);
}

static void repl_stdin(void)
{
    char *line = NULL, *acc = NULL;
    size_t linecap = 0;
    while (getline(&line, &linecap, stdin) >= 0) {
        if (!acc_append(&acc, line))
            continue;
        if (feed(acc) == FEED_DONE) {
            free(acc);
            acc = NULL;
        }
        psh_jobs_reap(); /* keep zombies from piling up */
    }
    if (acc) { /* EOF mid-construct */
        fprintf(stderr, "psh: syntax error: unexpected end of input\n");
        psh_last_status = 2;
        free(acc);
    }
    free(line);
}

int main(int argc, char **argv)
{
    /* The editor's cursor math decodes UTF-8 and asks wcwidth for
     * display columns (ə is 1, 🫛 is 2) — needs the user's locale. */
    setlocale(LC_CTYPE, "");

    /* psh -c 'commands' [args...] */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        psh_script_args = argv + 3;
        psh_script_argc = (size_t)(argc - 3);
        psh_jobs_init(false);
        return psh_run_string(argv[2]);
    }

    /* psh script [args...] — shebang lines are '#' comments already */
    if (argc >= 2) {
        psh_arg0 = argv[1];
        psh_script_args = argv + 2;
        psh_script_argc = (size_t)(argc - 2);
        psh_jobs_init(false);
        return psh_run_file(argv[1]);
    }

    bool interactive = isatty(STDIN_FILENO);
    if (interactive) {
        /* Flag-setting SIGINT handler (see on_sigint); quit ignored. */
        struct sigaction sa = { 0 };
        sa.sa_handler = on_sigint;
        sigaction(SIGINT, &sa, NULL);
        signal(SIGQUIT, SIG_IGN);
    }
    /* Non-interactive: default dispositions — a script dies on
     * Ctrl-C like any other program, which is what callers expect. */

    psh_jobs_init(interactive);

    if (interactive)
        repl_interactive();
    else
        repl_stdin();

    return psh_last_status;
}
