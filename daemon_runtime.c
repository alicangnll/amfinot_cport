/*
 * daemon_runtime.c
 *
 * Start Not-AMFI as a detached background process.
 * Pure C — no C++ or LLDB dependencies.
 *
 * Strategy:
 *   fork() → setsid() in child → exec() the same binary without "daemon"
 *   subcommand so the child runs the foreground bypass loop.
 */

#include "daemon_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Maximum number of arguments we will construct for the child */
#define MAX_CHILD_ARGS 2048

void start_daemon(const char  *executable,
                  const char **paths,
                  const char **cdhashes,
                  bool         verbose,
                  bool         allow_all)
{
    /*
     * Build the child argv:
     *   <executable> [--verbose] [--allow-all]
     *                [--path P1] [--path P2] ...
     *                [--cdhash C1] [--cdhash C2] ...
     */
    const char *child_argv[MAX_CHILD_ARGS];
    int argc = 0;

    child_argv[argc++] = executable;

    if (verbose)   child_argv[argc++] = "--verbose";
    if (allow_all) child_argv[argc++] = "--allow-all";

    if (paths) {
        for (int i = 0; paths[i] && argc + 2 < MAX_CHILD_ARGS; i++) {
            child_argv[argc++] = "--path";
            child_argv[argc++] = paths[i];
        }
    }

    if (cdhashes) {
        for (int i = 0; cdhashes[i] && argc + 2 < MAX_CHILD_ARGS; i++) {
            child_argv[argc++] = "--cdhash";
            child_argv[argc++] = cdhashes[i];
        }
    }

    child_argv[argc] = NULL;

    /* Build a human-readable command string for logging */
    char cmd_display[4096] = {0};
    for (int i = 0; i < argc; i++) {
        if (i > 0) strncat(cmd_display, " ", sizeof(cmd_display) - strlen(cmd_display) - 1);
        strncat(cmd_display, child_argv[i], sizeof(cmd_display) - strlen(cmd_display) - 1);
    }
    printf("Starting daemon with command: %s\n", cmd_display);

    pid_t pid = fork();
    if (pid < 0) {
        perror("Not-AMFI: fork");
        return;
    }

    if (pid == 0) {
        /* ---- Child ---- */
        /* Detach from the parent session */
        if (setsid() < 0)
            perror("Not-AMFI: setsid");

        /* Redirect stdio to /dev/null so the daemon is silent */
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        execv(executable, (char *const *)child_argv);
        /* execv only returns on error */
        _exit(127);
    }

    /* ---- Parent ---- */
    printf("Not-AMFI daemon started (pid: %d)\n", (int)pid);
}
