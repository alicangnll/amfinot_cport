/*
 * daemon_runtime.h
 *
 * Daemonisation support for Not-AMFI.
 * Mirrors the Python daemon_runtime.py module.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/*
 * Fork a detached background process that re-executes the same binary
 * (argv[0]) in foreground bypass mode with the supplied options.
 *
 * executable  - path to the Not-AMFI binary (argv[0])
 * paths       - NULL-terminated array of allowed path prefixes (may be NULL)
 * cdhashes    - NULL-terminated array of allowed cdhashes (may be NULL)
 * verbose     - forward --verbose flag to child
 * allow_all   - forward --allow-all flag to child
 *
 * Prints the child PID on success.
 */
void start_daemon(const char  *executable,
                  const char **paths,
                  const char **cdhashes,
                  bool         verbose,
                  bool         allow_all);

#ifdef __cplusplus
}
#endif
