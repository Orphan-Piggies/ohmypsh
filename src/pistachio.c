/*
 * pistachio.c — 🥜 the official home of easter pistachios.
 *
 * Rules of the module:
 *   1. Nothing in here may affect correctness. Delete this file,
 *      stub the three functions, and psh still works.
 *   2. New pistachios go here, not scattered through the codebase.
 *   3. XIRT.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <unistd.h>

#include "psh.h"

void psh_pistachio_hello(void)
{
    printf(PSH_NUT " \033[32mpsh %s\033[0m — the world's hardest shell\n",
           PSH_VERSION);
    printf("   type 'help' for builtins, Ctrl-D to leave the bag\n");
}

/*
 * Flavor line appended to command-not-found errors. Picked by child
 * PID, so which one you get is a small surprise each time.
 */
static const char *notfound[] = {
    "XIRT! this one wouldn't crack",
    "no such nut in this bag",
    "sealed shut — not even 9.8 newtons will open it",
    "checked the whole bag twice",
    "that's a walnut, we don't serve walnuts here",
};

const char *psh_pistachio_notfound(void)
{
    return notfound[(size_t)getpid() % (sizeof notfound / sizeof notfound[0])];
}

/*
 * The `crack` builtin. Undocumented on purpose (help lists it as ???).
 * Dispenses one pistachio and one piece of salty shell wisdom.
 */
static const char *wisdom[] = {
    "a quoted variable never splits. an unquoted one splits families.",
    "every process you've ever run was fork() and exec(). that's it. that's Unix.",
    "the shell that cracks under Ctrl-C was never a shell at all.",
    "PATH is just a list of places to look. so is life.",
    "exit status 0 is the only compliment Unix will ever give you.",
    "the hardest shells hold the best nuts.",
};

int psh_builtin_crack(char **argv)
{
    (void)argv;
    static size_t next; /* rotates across calls within one session */
    printf("    _____\n");
    printf("   /  ___\\   *crack*\n");
    printf("  |  /" PSH_NUT " |\n");
    printf("   \\ \\___/\n");
    printf("    \\____\\\n\n");
    printf("  \033[32m\"%s\"\033[0m\n",
           wisdom[next++ % (sizeof wisdom / sizeof wisdom[0])]);
    return 0;
}
