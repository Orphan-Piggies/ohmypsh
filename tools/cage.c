/*
 * cage — the armory's sandbox. 🫛🔒
 *
 * Run the sketchy thing FIRST, then decide:
 *
 *   cage ./install.psh           the filesystem is READ-ONLY except
 *                                a fresh scratch dir; the script
 *                                runs, its writes land in the cage
 *   cage -w build make           grant extra writable dirs
 *
 * Built on Landlock: unprivileged, kernel-enforced, pure syscalls —
 * no setuid, no containers, no dependencies, in keeping with the
 * house vow. The policy is deny-by-default for every write-shaped
 * access (create, write, truncate, delete, rename-in) across the
 * whole tree, then allowed back for: the scratch dir (also exported
 * as $CAGE_DIR and $TMPDIR, so well-behaved tools aim there by
 * themselves), each -w DIR, and /dev/null + /dev/tty (scripts
 * breathe through those). Reading and executing stay open — cage
 * guards your files, it is not a secrecy tool. Already-open fds
 * (your terminal) keep working: Landlock governs open(2), not
 * write(2) on inherited descriptors.
 *
 * The scratch dir SURVIVES the run — inspecting what the caged
 * thing tried to build is the point. Denied writes reach the child
 * as plain EACCES, so its own error messages name every blocked
 * path.
 *
 * No Landlock (kernel < 5.13, or LSM not enabled)? cage REFUSES,
 * loudly. A sandbox that silently doesn't sandbox is worse than
 * none. Check /sys/kernel/security/lsm for "landlock".
 *
 * Exit: the child's status (128+sig if signaled), 126/127 if the
 * command won't run, 125 if the cage itself cannot be built, 2 for
 * usage errors.
 */
#define _GNU_SOURCE

#ifndef __linux__
#include <stdio.h>
int main(void)
{
    fprintf(stderr, "cage: Landlock is Linux-only — no cage here\n");
    return 125;
}
#else

#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Landlock has had stable syscall numbers on every arch since 5.13;
 * glibc wrappers are newer than the kernels we want to support. */
static int ll_create_ruleset(const struct landlock_ruleset_attr *attr,
                             size_t size, __u32 flags)
{
    return (int)syscall(__NR_landlock_create_ruleset, attr, size, flags);
}
static int ll_add_rule(int fd, enum landlock_rule_type type,
                       const void *attr, __u32 flags)
{
    return (int)syscall(__NR_landlock_add_rule, fd, type, attr, flags);
}
static int ll_restrict_self(int fd, __u32 flags)
{
    return (int)syscall(__NR_landlock_restrict_self, fd, flags);
}

#define ACCESS_ABI1                                                     \
    (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |       \
     LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |       \
     LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |   \
     LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |       \
     LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |       \
     LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |    \
     LANDLOCK_ACCESS_FS_MAKE_SYM)

#define ACCESS_READ                                                     \
    (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE |        \
     LANDLOCK_ACCESS_FS_READ_DIR)

/* Allow `access` beneath `path` in the ruleset. Missing optional
 * paths (no /dev/tty in CI) are not an error. */
static int allow(int ruleset, const char *path, __u64 access,
                 bool optional)
{
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) {
        if (optional)
            return 0;
        fprintf(stderr, "cage: %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct landlock_path_beneath_attr pb = {
        .allowed_access = access,
        .parent_fd = fd,
    };
    int r = ll_add_rule(ruleset, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    if (r < 0)
        fprintf(stderr, "cage: rule for %s: %s\n", path, strerror(errno));
    close(fd);
    return r;
}

static void usage(void)
{
    fputs("cage — run a command against a read-only world 🔒\n"
          "usage: cage [-w DIR]... [-q] CMD [ARGS...]\n"
          "  -w DIR  also allow writes beneath DIR (repeatable)\n"
          "  -q      no banner\n"
          "Writes land in a fresh scratch dir (exported as $CAGE_DIR\n"
          "and $TMPDIR), kept after the run for inspection. Reading\n"
          "and executing stay open; /dev/null and /dev/tty stay\n"
          "writable. Everything else answers EACCES.\n",
          stderr);
}

int main(int argc, char **argv)
{
    const char *wdirs[32];
    size_t nw = 0;
    bool quiet = false;
    int opt;

    while ((opt = getopt(argc, argv, "+w:qh")) != -1) {
        switch (opt) {
        case 'w':
            if (nw >= sizeof wdirs / sizeof wdirs[0]) {
                fprintf(stderr, "cage: too many -w dirs\n");
                return 2;
            }
            wdirs[nw++] = optarg;
            break;
        case 'q': quiet = true; break;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }
    if (optind >= argc) {
        usage();
        return 2;
    }

    int abi = ll_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) {
        fprintf(stderr,
                "cage: no Landlock here (kernel < 5.13, or the LSM is "
                "off — check /sys/kernel/security/lsm).\n"
                "cage REFUSES to run uncaged: a sandbox that silently "
                "isn't one is worse than none.\n");
        return 125;
    }

    /* Handle every write-shaped access this kernel knows; newer
     * bits on older kernels would make create_ruleset EINVAL. */
    __u64 handled = ACCESS_ABI1;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2)
        handled |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3)
        handled |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif

    struct landlock_ruleset_attr attr = { .handled_access_fs = handled };
    int ruleset = ll_create_ruleset(&attr, sizeof attr, 0);
    if (ruleset < 0) {
        fprintf(stderr, "cage: create_ruleset: %s\n", strerror(errno));
        return 125;
    }

    /* The world: read and execute, nothing more. */
    if (allow(ruleset, "/", ACCESS_READ, false) < 0)
        return 125;

    /* The scratch: everything, and the child knows where it is. */
    char scratch[] = "/tmp/cage-XXXXXX";
    if (!mkdtemp(scratch)) {
        fprintf(stderr, "cage: mkdtemp: %s\n", strerror(errno));
        return 125;
    }
    if (allow(ruleset, scratch, handled, false) < 0)
        return 125;
    setenv("CAGE_DIR", scratch, 1);
    setenv("TMPDIR", scratch, 1);

    /* Breathing holes. */
    __u64 devwrite = LANDLOCK_ACCESS_FS_WRITE_FILE;
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3)
        devwrite |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    if (allow(ruleset, "/dev/null", devwrite, true) < 0)
        return 125;
    if (allow(ruleset, "/dev/tty", devwrite, true) < 0)
        return 125;

    /* The user's chosen gates. */
    for (size_t i = 0; i < nw; i++)
        if (allow(ruleset, wdirs[i], handled, false) < 0)
            return 125;

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 ||
        ll_restrict_self(ruleset, 0) < 0) {
        fprintf(stderr, "cage: restrict_self: %s\n", strerror(errno));
        return 125;
    }
    close(ruleset);

    if (!quiet)
        fprintf(stderr, "cage: 🔒 read-only world · scratch %s (kept)\n",
                scratch);

    execvp(argv[optind], argv + optind);
    fprintf(stderr, "cage: %s: %s\n", argv[optind], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}

#endif /* __linux__ */
