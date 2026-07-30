/*
 * Not-AMFI.cpp
 *
 * Entry point for the Not-AMFI C/C++ port.
 *
 * Mirrors __main__.py:
 *   Not-AMFI [OPTIONS]            → foreground bypass (run_bypass)
 *   Not-AMFI daemon [OPTIONS]     → background daemon (start_daemon)
 *   Not-AMFI add-path    <path>   → persistent config
 *   Not-AMFI remove-path <path>   → persistent config
 *   Not-AMFI add-cdhash  <hash>   → persistent config
 *   Not-AMFI remove-cdhash <hash> → persistent config
 *
 * Argument parsing: getopt_long (POSIX, available on macOS).
 */

#include "bypass_runtime.h"
#include "config_store.h"
#include "daemon_runtime.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Usage                                                                */
/* ------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    printf(
        "Usage: %s [OPTIONS] [COMMAND [ARGS...]]\n"
        "\n"
        "A simple utility for bypassing amfid signature verification.\n"
        "\n"
        "Options:\n"
        "  -p, --path <path>      path of executable to allow\n"
        "                         (can be specified multiple times;\n"
        "                          merged with ~/.not-amfi/paths)\n"
        "  -c, --cdhash <hash>    cdhash of executable to allow\n"
        "                         (can be specified multiple times;\n"
        "                          merged with ~/.not-amfi/cdhashes)\n"
        "  -v, --verbose          enable verbose output\n"
        "      --allow-all        allow all validations to pass\n"
        "  -h, --help             show this help and exit\n"
        "\n"
        "Commands:\n"
        "  daemon                 start Not-AMFI in daemon mode\n"
        "  add-path    <path>     add allowed path prefix to persistent config\n"
        "  remove-path <path>     remove allowed path prefix from persistent config\n"
        "  add-cdhash  <hash>     add allowed cdhash to persistent config\n"
        "  remove-cdhash <hash>   remove allowed cdhash from persistent config\n"
        "\n"
        "When no command is given, Not-AMFI runs in foreground bypass mode.\n",
        prog);
}

/* ------------------------------------------------------------------ */
/* Dynamic string list                                                  */
/* ------------------------------------------------------------------ */

#define MAX_CLI_ENTRIES 256

typedef struct {
    const char *data[MAX_CLI_ENTRIES + 1]; /* NULL-terminated */
    size_t      count;
} CliList;

static void cli_list_add(CliList *list, const char *value)
{
    if (list->count >= MAX_CLI_ENTRIES) {
        fprintf(stderr, "Not-AMFI: too many entries (max %d)\n", MAX_CLI_ENTRIES);
        return;
    }
    list->data[list->count++] = value;
    list->data[list->count]   = NULL;
}

/* ------------------------------------------------------------------ */
/* Option parsing helpers                                               */
/* ------------------------------------------------------------------ */

static const struct option long_opts[] = {
    { "path",         required_argument, nullptr, 'p' },
    { "cdhash",       required_argument, nullptr, 'c' },
    { "verbose",      no_argument,       nullptr, 'v' },
    { "allow-all",    no_argument,       nullptr, 'A' },
    { "help",         no_argument,       nullptr, 'h' },
    { nullptr,        0,                 nullptr,  0  },
};

typedef struct {
    CliList paths;
    CliList cdhashes;
    bool    verbose;
    bool    allow_all;
} GlobalOpts;

/*
 * Parse global options from argv, stopping at the first non-option
 * argument (the subcommand name) or "--".
 *
 * Returns the index of the first non-option argument, or -1 on error.
 */
static int parse_global_opts(int argc, char *argv[], GlobalOpts *opts)
{
    memset(opts, 0, sizeof(*opts));
    int ch;
    while ((ch = getopt_long(argc, argv, "+p:c:vh", long_opts, nullptr)) != -1) {
        switch (ch) {
        case 'p': cli_list_add(&opts->paths,    optarg); break;
        case 'c': cli_list_add(&opts->cdhashes, optarg); break;
        case 'v': opts->verbose   = true;                break;
        case 'A': opts->allow_all = true;                break;
        case 'h': print_usage(argv[0]); exit(0);
        default:
            fprintf(stderr, "Run '%s --help' for usage.\n", argv[0]);
            return -1;
        }
    }
    return optind; /* index of first remaining arg */
}

/* ------------------------------------------------------------------ */
/* Subcommand: daemon                                                   */
/* ------------------------------------------------------------------ */

static int cmd_daemon(int argc, char *argv[], GlobalOpts *gopts)
{
    /* Parse the same options again after the "daemon" subcommand */
    optind = 1;   /* reset getopt */
    GlobalOpts local = *gopts;

    int ch;
    while ((ch = getopt_long(argc, argv, "+p:c:vh", long_opts, nullptr)) != -1) {
        switch (ch) {
        case 'p': cli_list_add(&local.paths,    optarg); break;
        case 'c': cli_list_add(&local.cdhashes, optarg); break;
        case 'v': local.verbose   = true;                break;
        case 'A': local.allow_all = true;                break;
        case 'h': print_usage(argv[0]); return 0;
        default:  return 1;
        }
    }

    start_daemon(argv[0],
                 local.paths.data,
                 local.cdhashes.data,
                 local.verbose,
                 local.allow_all);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Subcommand: add-path / remove-path / add-cdhash / remove-cdhash     */
/* ------------------------------------------------------------------ */

static int cmd_config(const char *subcmd, const char *arg,
                      const char *paths_file, const char *cdhashes_file,
                      const char *config_dir)
{
    /* Ensure config directory exists */
    AmfidontConfig *cfg = load_persistent_config(config_dir, paths_file, cdhashes_file);
    if (!cfg) {
        fprintf(stderr, "Not-AMFI: failed to initialise config directory\n");
        return 1;
    }
    free_config(cfg);

    bool is_path   = (strncmp(subcmd, "add-path",      8) == 0 ||
                      strncmp(subcmd, "remove-path",   11) == 0);
    bool is_add    = (subcmd[0] == 'a');
    const char *filepath = is_path ? paths_file : cdhashes_file;
    const char *kind     = is_path ? "path" : "cdhash";

    if (is_add) {
        if (add_config_entry(filepath, arg))
            printf("added %s: %s\n", kind, arg);
        else
            printf("%s already present: %s\n", kind, arg);
    } else {
        if (remove_config_entry(filepath, arg))
            printf("removed %s: %s\n", kind, arg);
        else
            printf("%s not found: %s\n", kind, arg);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 1) return 1;

    /* Expand config directory paths once at startup */
    char config_dir[MAX_ENTRY_LEN];
    char paths_file[MAX_ENTRY_LEN];
    char cdhashes_file[MAX_ENTRY_LEN];
    config_paths_init(config_dir, paths_file, cdhashes_file, sizeof(config_dir));

    /* Parse global options */
    GlobalOpts gopts;
    int subcmd_idx = parse_global_opts(argc, argv, &gopts);
    if (subcmd_idx < 0)
        return 1;

    /* No subcommand → foreground bypass */
    if (subcmd_idx >= argc) {
        run_bypass(gopts.paths.data, gopts.cdhashes.data,
                   gopts.verbose, gopts.allow_all);
        return 0;
    }

    const char *subcmd = argv[subcmd_idx];

    /* Remaining args after the subcommand */
    int    sub_argc = argc - subcmd_idx;
    char **sub_argv = argv + subcmd_idx;

    /* --- daemon --- */
    if (strcmp(subcmd, "daemon") == 0) {
        optind = 1;
        return cmd_daemon(sub_argc, sub_argv, &gopts);
    }

    /* --- config subcommands (require exactly one positional arg) --- */
    if (strcmp(subcmd, "add-path")      == 0 ||
        strcmp(subcmd, "remove-path")   == 0 ||
        strcmp(subcmd, "add-cdhash")    == 0 ||
        strcmp(subcmd, "remove-cdhash") == 0)
    {
        if (sub_argc < 2) {
            fprintf(stderr, "Not-AMFI: %s requires an argument\n", subcmd);
            return 1;
        }
        return cmd_config(subcmd, sub_argv[1],
                          paths_file, cdhashes_file, config_dir);
    }

    fprintf(stderr, "Not-AMFI: unknown command '%s'\n"
                    "Run '%s --help' for usage.\n",
            subcmd, argv[0]);
    return 1;
}
