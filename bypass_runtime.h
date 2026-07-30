/*
 * bypass_runtime.h
 *
 * LLDB-based amfid signature-validation bypass.
 * Mirrors the Python bypass_runtime.py module.
 *
 * Uses the LLDB C++ SB API (LLDB.framework on macOS).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/*
 * Attach to /usr/libexec/amfid, install the validation breakpoint,
 * and run the foreground bypass loop until interrupted (SIGINT/SIGTERM).
 *
 * paths      - NULL-terminated array of allowed path prefixes (may be NULL)
 * cdhashes   - NULL-terminated array of allowed cdhashes (may be NULL)
 * verbose    - enable informative runtime logging
 * allow_all  - bypass all validations unconditionally
 */
void run_bypass(const char **paths,
                const char **cdhashes,
                bool         verbose,
                bool         allow_all);

#ifdef __cplusplus
}
#endif
