/*
 * bypass_runtime.cpp
 *
 * LLDB-based amfid bypass loop.
 *
 * Mirrors bypass_runtime.py in full fidelity:
 *
 *   1. Create an SBDebugger (synchronous mode).
 *   2. Attach to /usr/libexec/amfid by name.
 *   3. Install a breakpoint on -[AMFIPathValidator_macos validateWithError:].
 *   4. Loop:
 *        a. Continue the process.
 *        b. On breakpoint hit, call validate_hook():
 *             - Read the "self" register (x0 on arm64 / rdi on x86_64).
 *             - Step out of the current frame.
 *             - Read the return register (x0 / rax).
 *             - Evaluate ObjC expressions to retrieve path, cdhash, isValid.
 *             - If the validator is invalid but path/cdhash is allow-listed,
 *               patch the return register to 1 (success).
 *        c. Hot-reload config if ~/.not-amfi file mtimes changed.
 *
 * Dependencies: LLDB.framework (Xcode SharedFrameworks).
 */

#include "bypass_runtime.h"
#include "config_store.h"

/* LLDB C++ SB API — Homebrew LLVM: brew install llvm */
#include <lldb/API/LLDB.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

static const char *AMFID_PATH       = "/usr/libexec/amfid";
static const char *BREAKPOINT_NAME  = "-[AMFIPathValidator_macos validateWithError:]";

/*
 * Per-architecture register pair: (return_register, self_register).
 *
 * arm64  : self arrives in x0; return value also in x0 after step-out.
 * x86_64 : self arrives in rdi; return value in rax after step-out.
 */
typedef struct { const char *ret_reg; const char *self_reg; } ArchRegs;

static ArchRegs arch_regs_for_triple(const char *triple)
{
    if (triple && strstr(triple, "arm64"))
        return { "x0", "x0" };
    if (triple && strstr(triple, "x86_64"))
        return { "rax", "rdi" };
    fprintf(stderr, "Not-AMFI: unsupported architecture triple: %s\n",
            triple ? triple : "(null)");
    return { "x0", "x0" };   /* best-effort fallback */
}

/* ------------------------------------------------------------------ */
/* Validator inspection                                                 */
/* ------------------------------------------------------------------ */

/*
 * Parsed fields from an AMFIPathValidator_macos object.
 */
typedef struct {
    char path[MAX_ENTRY_LEN];
    char cdhash[256];
    bool is_valid;
} ValidatorInfo;

/*
 * Evaluate an ObjC expression against the LLDB target.
 * Returns the SBValue; caller checks IsValid().
 */
static lldb::SBValue eval(lldb::SBTarget &target, const char *expr)
{
    lldb::SBExpressionOptions opts;
    opts.SetLanguage(lldb::eLanguageTypeObjC);
    return target.EvaluateExpression(expr, opts);
}

/*
 * Read path, cdhash, and isValid from an AMFIPathValidator_macos pointer.
 *
 * validator_ptr : hex string representation of the ObjC object pointer
 *                 (e.g. "0x00007f8123456789") obtained from the self register.
 *
 * Returns true on success, false if any expression evaluation failed.
 */
static bool dump_validator(lldb::SBTarget &target,
                            const char     *validator_ptr,
                            ValidatorInfo  *out)
{
    char expr[MAX_ENTRY_LEN];

    /* --- isValid --- */
    snprintf(expr, sizeof(expr),
             "(BOOL)[(id)%s isValid]", validator_ptr);
    lldb::SBValue v_valid = eval(target, expr);
    if (!v_valid.IsValid()) {
        fprintf(stderr, "Not-AMFI: failed to evaluate isValid\n");
        return false;
    }
    out->is_valid = (bool)v_valid.GetValueAsUnsigned(0);

    /* --- codePath (NSURL → file path string) --- */
    snprintf(expr, sizeof(expr),
             "(NSURL*)[(id)%s codePath]", validator_ptr);
    lldb::SBValue v_path = eval(target, expr);
    if (!v_path.IsValid()) {
        fprintf(stderr, "Not-AMFI: failed to evaluate codePath\n");
        return false;
    }
    const char *path_desc = v_path.GetObjectDescription();
    if (!path_desc) {
        fprintf(stderr, "Not-AMFI: codePath description is null\n");
        return false;
    }
    /* Description is "file:///path/to/binary" — strip "file://" prefix */
    const char *file_prefix = "file://";
    if (strncmp(path_desc, file_prefix, strlen(file_prefix)) != 0) {
        fprintf(stderr, "Not-AMFI: only file:// code paths are supported (got %s)\n",
                path_desc);
        return false;
    }
    snprintf(out->path, sizeof(out->path), "%s", path_desc + strlen(file_prefix));

    /* --- cdhashAsData (NSData → hex string) --- */
    snprintf(expr, sizeof(expr),
             "(NSData*)[(id)%s cdhashAsData]", validator_ptr);
    lldb::SBValue v_hash = eval(target, expr);
    if (!v_hash.IsValid()) {
        fprintf(stderr, "Not-AMFI: failed to evaluate cdhashAsData\n");
        return false;
    }
    const char *hash_desc = v_hash.GetObjectDescription();
    if (!hash_desc) {
        fprintf(stderr, "Not-AMFI: cdhashAsData description is null\n");
        return false;
    }
    /*
     * NSData -description returns "<aabb ccdd ...>".
     * Strip enclosing '<' '>' and remove spaces to get a compact hex string.
     */
    size_t j = 0;
    for (size_t i = 0; hash_desc[i] && j + 1 < sizeof(out->cdhash); i++) {
        char c = hash_desc[i];
        if (c == '<' || c == '>' || c == ' ')
            continue;
        out->cdhash[j++] = c;
    }
    out->cdhash[j] = '\0';

    return true;
}

/* ------------------------------------------------------------------ */
/* validate_hook                                                        */
/* ------------------------------------------------------------------ */

/*
 * Called on each breakpoint hit.
 *
 * Reads the self pointer, steps out of the validator frame, inspects
 * path/cdhash/isValid, and patches the return register to 1 if the
 * validator should be allowed through.
 */
static void validate_hook(lldb::SBTarget  &target,
                           lldb::SBThread   thread,
                           const char     **allowed_paths,
                           size_t           path_count,
                           const char     **allowed_cdhashes,
                           size_t           cdhash_count,
                           bool             verbose,
                           bool             allow_all)
{
    const char *triple = target.GetTriple();
    ArchRegs regs = arch_regs_for_triple(triple);

    /* Grab the self register from frame 0 before stepping out */
    lldb::SBFrame frame = thread.GetFrameAtIndex(0);
    if (!frame.IsValid()) {
        fprintf(stderr, "Not-AMFI: invalid frame at breakpoint\n");
        return;
    }

    lldb::SBValue self_val = frame.FindRegister(regs.self_reg);
    if (!self_val.IsValid()) {
        fprintf(stderr, "Not-AMFI: could not read %s register\n", regs.self_reg);
        return;
    }
    const char *validator_ptr = self_val.GetValue();
    if (!validator_ptr) {
        fprintf(stderr, "Not-AMFI: self register value is null\n");
        return;
    }

    /* Step out of the validator frame so the return value is available */
    lldb::SBError step_err;
    thread.StepOutOfFrame(frame, step_err);
    if (step_err.Fail()) {
        fprintf(stderr, "Not-AMFI: StepOutOfFrame failed: %s\n",
                step_err.GetCString());
        return;
    }

    /* Pointer to the return-value register in the caller's frame */
    lldb::SBFrame ret_frame = thread.GetFrameAtIndex(0);
    lldb::SBValue ret_reg   = ret_frame.FindRegister(regs.ret_reg);

    /* --- allow_all fast path --- */
    if (allow_all) {
        if (verbose)
            printf("Not-AMFI: allowed (--allow-all)\n");
        ret_reg.SetValueFromCString("1");
        return;
    }

    /* Inspect the validator object */
    ValidatorInfo info;
    memset(&info, 0, sizeof(info));
    if (!dump_validator(target, validator_ptr, &info)) {
        /* dump_validator already printed the error */
        return;
    }

    /* Only intervene if amfid itself marked the binary as invalid */
    if (info.is_valid)
        return;

    /* Check cdhash allow-list */
    for (size_t i = 0; i < cdhash_count; i++) {
        if (allowed_cdhashes[i] &&
            strcasecmp(info.cdhash, allowed_cdhashes[i]) == 0)
        {
            if (verbose)
                printf("Not-AMFI: allowed via cdhash %s\n", info.cdhash);
            ret_reg.SetValueFromCString("1");
            return;
        }
    }

    /* Check path prefix allow-list */
    for (size_t i = 0; i < path_count; i++) {
        if (allowed_paths[i] &&
            strncmp(info.path, allowed_paths[i], strlen(allowed_paths[i])) == 0)
        {
            if (verbose)
                printf("Not-AMFI: allowed via path %s\n", info.path);
            ret_reg.SetValueFromCString("1");
            return;
        }
    }

    if (verbose) {
        printf("Not-AMFI: not patching (not allow-listed):\n");
        printf("  path   : %s\n", info.path);
        printf("  cdhash : %s\n", info.cdhash);
    }
}

/* ------------------------------------------------------------------ */
/* Merge CLI + persistent config into flat arrays                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char **data;
    size_t       count;
} StrArray;

/*
 * Build a merged (CLI + persistent config) StrArray on the heap.
 * CLI entries come first; duplicates are not filtered.
 */
static StrArray merge_string_arrays(const char **cli_arr,
                                     char       **cfg_arr,
                                     size_t       cfg_count)
{
    size_t cli_count = 0;
    if (cli_arr)
        while (cli_arr[cli_count]) cli_count++;

    size_t total = cli_count + cfg_count;
    const char **out = (const char **)calloc(total + 1, sizeof(char *));
    size_t idx = 0;
    for (size_t i = 0; i < cli_count;  i++) out[idx++] = cli_arr[i];
    for (size_t i = 0; i < cfg_count;  i++) out[idx++] = cfg_arr[i];
    return { out, total };
}

/* ------------------------------------------------------------------ */
/* verbose helpers                                                      */
/* ------------------------------------------------------------------ */

static void print_string_list(const char *header,
                               const char **arr,
                               size_t       count)
{
    printf("  %s:\n", header);
    if (count == 0) {
        printf("    - (none)\n");
        return;
    }
    for (size_t i = 0; i < count; i++)
        printf("    - %s\n", arr[i]);
}

/* ------------------------------------------------------------------ */
/* bypass_loop                                                          */
/* ------------------------------------------------------------------ */

/*
 * Main event loop:
 *   1. Continue the process.
 *   2. Check if a thread stopped on our breakpoint → call validate_hook().
 *   3. Hot-reload config if mtime changed.
 *   4. Repeat.
 */
static void bypass_loop(lldb::SBProcess  &process,
                         lldb::SBTarget   &target,
                         const char      **cli_paths,
                         const char      **cli_cdhashes,
                         const char       *paths_file,
                         const char       *cdhashes_file,
                         const char       *config_dir,
                         bool              verbose,
                         bool              allow_all)
{
    /* Load initial persistent config */
    AmfidontConfig *cfg = load_persistent_config(config_dir, paths_file, cdhashes_file);
    ConfigMtimeState mtime = config_modified_time_state(paths_file, cdhashes_file);

    StrArray paths    = merge_string_arrays(cli_paths,    cfg ? cfg->paths    : nullptr,
                                                         cfg ? cfg->path_count   : 0);
    StrArray cdhashes = merge_string_arrays(cli_cdhashes, cfg ? cfg->cdhashes  : nullptr,
                                                         cfg ? cfg->cdhash_count : 0);

    if (verbose) {
        printf("Not-AMFI: running configuration:\n");
        printf("  Allow all : %s\n", allow_all ? "yes" : "no");
        print_string_list("Paths",    paths.data,    paths.count);
        print_string_list("CDHashes", cdhashes.data, cdhashes.count);
    }

    for (;;) {
        /* Continue the attached process */
        lldb::SBError cont_err = process.Continue();
        if (cont_err.Fail()) {
            fprintf(stderr, "Not-AMFI: process.Continue() failed: %s\n",
                    cont_err.GetCString());
            break;
        }

        /* Sanity-check process state */
        lldb::StateType state = process.GetState();
        if (state != lldb::eStateRunning  &&
            state != lldb::eStateStopped  &&
            state != lldb::eStateSuspended)
        {
            fprintf(stderr, "Not-AMFI: unexpected process state %d — exiting loop\n",
                    (int)state);
            break;
        }

        /* Hot-reload config if files changed */
        ConfigMtimeState cur_mtime = config_modified_time_state(paths_file, cdhashes_file);
        if (!config_mtime_equal(&mtime, &cur_mtime)) {
            free(paths.data);
            free(cdhashes.data);
            free_config(cfg);

            cfg    = load_persistent_config(config_dir, paths_file, cdhashes_file);
            mtime  = cur_mtime;
            paths    = merge_string_arrays(cli_paths,    cfg ? cfg->paths    : nullptr,
                                                         cfg ? cfg->path_count   : 0);
            cdhashes = merge_string_arrays(cli_cdhashes, cfg ? cfg->cdhashes  : nullptr,
                                                         cfg ? cfg->cdhash_count : 0);
            if (verbose)
                printf("Not-AMFI: reloaded configuration from ~/.not-amfi\n");
        }

        /* Find the thread stopped on our breakpoint */
        uint32_t num_threads = process.GetNumThreads();
        for (uint32_t i = 0; i < num_threads; i++) {
            lldb::SBThread thread = process.GetThreadAtIndex(i);
            if (thread.GetStopReason() == lldb::eStopReasonBreakpoint) {
                validate_hook(target, thread,
                              paths.data,    paths.count,
                              cdhashes.data, cdhashes.count,
                              verbose, allow_all);
                break;
            }
        }
    }

    free(paths.data);
    free(cdhashes.data);
    free_config(cfg);
}

/* ------------------------------------------------------------------ */
/* Signal handling                                                      */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_interrupted = 0;

static void sigint_handler(int /*sig*/) { g_interrupted = 1; }

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */

void run_bypass(const char **paths,
                const char **cdhashes,
                bool         verbose,
                bool         allow_all)
{
    /* Expand config paths */
    char config_dir[MAX_ENTRY_LEN];
    char paths_file[MAX_ENTRY_LEN];
    char cdhashes_file[MAX_ENTRY_LEN];
    config_paths_init(config_dir, paths_file, cdhashes_file, sizeof(config_dir));

    /* Initialise LLDB */
    lldb::SBDebugger::Initialize();
    lldb::SBDebugger debugger = lldb::SBDebugger::Create();
    debugger.SetAsync(false);

    /* Create an empty target — we attach by process name */
    lldb::SBTarget target = debugger.CreateTarget("");
    if (!target.IsValid()) {
        fprintf(stderr, "Not-AMFI: failed to create LLDB target\n");
        lldb::SBDebugger::Terminate();
        return;
    }

    /* Attach to amfid */
    lldb::SBError     attach_err;
    lldb::SBListener  listener = debugger.GetListener();
    lldb::SBProcess   process  = target.AttachToProcessWithName(
                                     listener,
                                     AMFID_PATH,
                                     /*waitForLaunch=*/false,
                                     attach_err);

    if (attach_err.Fail() || !process.IsValid()) {
        fprintf(stderr, "Not-AMFI: failed to attach to %s: %s\n"
                        "          (try running as root)\n",
                AMFID_PATH,
                attach_err.IsValid() ? attach_err.GetCString() : "unknown error");
        lldb::SBDebugger::Terminate();
        return;
    }

    if (verbose)
        printf("Not-AMFI: attached to %s (pid %d)\n",
               AMFID_PATH, (int)process.GetProcessID());

    /* Install the validation breakpoint */
    lldb::SBBreakpoint bp = target.BreakpointCreateByName(BREAKPOINT_NAME);
    if (!bp.IsValid()) {
        fprintf(stderr, "Not-AMFI: failed to create breakpoint on %s\n",
                BREAKPOINT_NAME);
    } else if (verbose) {
        printf("Not-AMFI: breakpoint installed on %s\n", BREAKPOINT_NAME);
    }

    /* Install SIGINT handler for graceful detach on Ctrl-C */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    /* Run the bypass event loop */
    bypass_loop(process, target,
                paths, cdhashes,
                paths_file, cdhashes_file, config_dir,
                verbose, allow_all);

    /* Detach cleanly on exit */
    if (verbose)
        printf("Not-AMFI: stopping (detaching from amfid)...\n");

    lldb::SBError detach_err = process.Detach();
    if (detach_err.Fail())
        fprintf(stderr, "Not-AMFI: detach warning: %s\n", detach_err.GetCString());

    lldb::SBDebugger::Destroy(debugger);
    lldb::SBDebugger::Terminate();
}
