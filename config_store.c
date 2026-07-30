/*
 * config_store.c
 *
 * Implementation of persistent allow-list management.
 * Pure C — no C++ or LLDB dependencies.
 */

#include "config_store.h"

#include <errno.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Return the home directory, preferring $HOME over the password database.
 */
static const char *get_home_dir(void)
{
    const char *home = getenv("HOME");
    if (home && home[0] != '\0')
        return home;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/tmp";
}

/*
 * Ensure that a directory (and all parents) exists.
 * Returns 0 on success, -1 on error.
 */
static int mkdir_p(const char *path)
{
    char tmp[MAX_ENTRY_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/*
 * Read newline-separated entries from a file into a heap-allocated
 * array of strings.  Creates an empty file if it does not exist.
 *
 * out_entries - receives a heap-allocated array of heap-allocated strings
 * out_count   - receives the number of valid entries
 *
 * Returns 0 on success, -1 on error.
 */
static int read_list_file(const char *path,
                          char      ***out_entries,
                          size_t      *out_count)
{
    *out_entries = NULL;
    *out_count   = 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        /* Create empty file */
        f = fopen(path, "w");
        if (!f) return -1;
        fclose(f);
        *out_entries = (char **)calloc(1, sizeof(char *));
        return 0;
    }

    char  **entries  = (char **)calloc(MAX_ENTRIES, sizeof(char *));
    size_t  count    = 0;
    char    line[MAX_ENTRY_LEN];

    while (fgets(line, sizeof(line), f) && count < MAX_ENTRIES) {
        /* Strip trailing newline / carriage return */
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln - 1] == '\n' || line[ln - 1] == '\r'))
            line[--ln] = '\0';

        /* Skip blank lines */
        if (ln == 0)
            continue;

        entries[count++] = strdup(line);
    }

    fclose(f);
    *out_entries = entries;
    *out_count   = count;
    return 0;
}

/*
 * Write a string array back to a list file, one entry per line.
 * Creates parent directories as needed.
 */
static int write_list_file(const char  *path,
                           char       **entries,
                           size_t       count)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (size_t i = 0; i < count; i++)
        fprintf(f, "%s\n", entries[i]);
    fclose(f);
    return 0;
}

/*
 * Return file mtime in nanoseconds, or 0 if the file does not exist.
 * Sets *exists to 1 if the file is present, 0 otherwise.
 */
static long file_mtime_ns(const char *path, int *exists)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        *exists = 0;
        return 0;
    }
    *exists = 1;
#ifdef __APPLE__
    return (long)st.st_mtimespec.tv_sec * 1000000000L
         + (long)st.st_mtimespec.tv_nsec;
#else
    return (long)st.st_mtim.tv_sec * 1000000000L
         + (long)st.st_mtim.tv_nsec;
#endif
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void config_paths_init(char   *out_config_dir,
                       char   *out_paths_file,
                       char   *out_cdhashes_file,
                       size_t  len)
{
    const char *home = get_home_dir();
    snprintf(out_config_dir,    len, "%s/.not-amfi",          home);
    snprintf(out_paths_file,    len, "%s/.not-amfi/paths",    home);
    snprintf(out_cdhashes_file, len, "%s/.not-amfi/cdhashes", home);
}

NotAmfiConfig *load_persistent_config(const char *config_dir,
                                        const char *paths_file,
                                        const char *cdhashes_file)
{
    if (mkdir_p(config_dir) != 0) {
        fprintf(stderr, "Not-AMFI: cannot create config directory %s: %s\n",
                config_dir, strerror(errno));
        return NULL;
    }

    NotAmfiConfig *cfg = (NotAmfiConfig *)calloc(1, sizeof(NotAmfiConfig));
    if (!cfg) return NULL;

    if (read_list_file(paths_file,    &cfg->paths,    &cfg->path_count)    != 0 ||
        read_list_file(cdhashes_file, &cfg->cdhashes, &cfg->cdhash_count)  != 0) {
        free_config(cfg);
        return NULL;
    }

    return cfg;
}

void free_config(NotAmfiConfig *cfg)
{
    if (!cfg) return;
    for (size_t i = 0; i < cfg->path_count;    i++) free(cfg->paths[i]);
    for (size_t i = 0; i < cfg->cdhash_count;  i++) free(cfg->cdhashes[i]);
    free(cfg->paths);
    free(cfg->cdhashes);
    free(cfg);
}

bool add_config_entry(const char *filepath, const char *value)
{
    char  **entries = NULL;
    size_t  count   = 0;

    read_list_file(filepath, &entries, &count);

    /* Check if already present */
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i], value) == 0) {
            for (size_t j = 0; j < count; j++) free(entries[j]);
            free(entries);
            return false;
        }
    }

    /* Append and write back */
    entries[count++] = strdup(value);
    write_list_file(filepath, entries, count);

    for (size_t i = 0; i < count; i++) free(entries[i]);
    free(entries);
    return true;
}

bool remove_config_entry(const char *filepath, const char *value)
{
    char  **entries = NULL;
    size_t  count   = 0;

    read_list_file(filepath, &entries, &count);

    /* Build new list without the target value */
    char  **kept      = (char **)calloc(count, sizeof(char *));
    size_t  kept_count = 0;
    bool    found     = false;

    for (size_t i = 0; i < count; i++) {
        if (!found && strcmp(entries[i], value) == 0) {
            found = true;
            free(entries[i]);
        } else {
            kept[kept_count++] = entries[i];
        }
    }

    if (found)
        write_list_file(filepath, kept, kept_count);

    for (size_t i = 0; i < kept_count; i++) free(kept[i]);
    free(kept);
    free(entries);
    return found;
}

ConfigMtimeState config_modified_time_state(const char *paths_file,
                                             const char *cdhashes_file)
{
    ConfigMtimeState state;
    state.paths_mtime_ns    = file_mtime_ns(paths_file,    &state.paths_exists);
    state.cdhashes_mtime_ns = file_mtime_ns(cdhashes_file, &state.cdhashes_exists);
    return state;
}

bool config_mtime_equal(const ConfigMtimeState *a, const ConfigMtimeState *b)
{
    return a->paths_mtime_ns    == b->paths_mtime_ns    &&
           a->cdhashes_mtime_ns == b->cdhashes_mtime_ns &&
           a->paths_exists      == b->paths_exists       &&
           a->cdhashes_exists   == b->cdhashes_exists;
}
