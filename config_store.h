/*
 * config_store.h
 *
 * Persistent allow-list management for Not-AMFI.
 * Mirrors the Python config_store.py module.
 *
 * Config directory : ~/.not-amfi/
 * Paths file       : ~/.not-amfi/paths
 * CDHashes file    : ~/.not-amfi/cdhashes
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

#define MAX_ENTRY_LEN  4096
#define MAX_ENTRIES    1024

/* --------------------------------------------------------------------
 * Types
 * -------------------------------------------------------------------- */

/*
 * Loaded allow-list configuration.
 * Caller owns and must free with free_config().
 */
typedef struct {
    char  **paths;
    size_t  path_count;
    char  **cdhashes;
    size_t  cdhash_count;
} NotAmfiConfig;

/*
 * Snapshot of config file modification times.
 * Used to detect hot-reload triggers.
 */
typedef struct {
    long paths_mtime_ns;
    long cdhashes_mtime_ns;
    int  paths_exists;
    int  cdhashes_exists;
} ConfigMtimeState;

/* --------------------------------------------------------------------
 * Initialisation
 * -------------------------------------------------------------------- */

/*
 * Expand ~/.not-amfi and populate the three output buffers.
 * Must be called once before any other config_* function.
 *
 * out_config_dir   - receives expanded config directory path
 * out_paths_file   - receives full path to the paths list file
 * out_cdhashes_file- receives full path to the cdhashes list file
 * len              - size of each output buffer
 */
void config_paths_init(char *out_config_dir,
                       char *out_paths_file,
                       char *out_cdhashes_file,
                       size_t len);

/* --------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------- */

/*
 * Load persisted path and cdhash allow-lists from disk.
 * Creates config_dir and empty list files if they do not exist.
 *
 * Returns a heap-allocated NotAmfiConfig, or NULL on fatal error.
 * The caller is responsible for calling free_config().
 */
NotAmfiConfig *load_persistent_config(const char *config_dir,
                                       const char *paths_file,
                                       const char *cdhashes_file);

/* Free a NotAmfiConfig returned by load_persistent_config(). */
void free_config(NotAmfiConfig *cfg);

/* --------------------------------------------------------------------
 * Modification
 * -------------------------------------------------------------------- */

/*
 * Add value to the newline-delimited list file at filepath.
 *
 * Returns true  if the entry was added.
 * Returns false if it was already present.
 */
bool add_config_entry(const char *filepath, const char *value);

/*
 * Remove value from the newline-delimited list file at filepath.
 *
 * Returns true  if the entry was found and removed.
 * Returns false if the entry was not present.
 */
bool remove_config_entry(const char *filepath, const char *value);

/* --------------------------------------------------------------------
 * Hot-reload support
 * -------------------------------------------------------------------- */

/*
 * Return a snapshot of modification times for the two config files.
 * A change in the snapshot signals that the config should be reloaded.
 */
ConfigMtimeState config_modified_time_state(const char *paths_file,
                                             const char *cdhashes_file);

/*
 * Return true if the two snapshots are identical (no change).
 */
bool config_mtime_equal(const ConfigMtimeState *a, const ConfigMtimeState *b);

#ifdef __cplusplus
}
#endif
