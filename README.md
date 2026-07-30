# Not-AMFI — C/C++ Port

**Not-AMFI** attaches to `/usr/libexec/amfid` via the LLDB C++ SB API and intercepts Apple Mobile File Integrity (AMFI) signature validation, allowing explicitly allow-listed binaries to pass regardless of their code signature state.

> **Original project:** Python — uses the `lldb` Python scripting bridge (`import lldb`).  
> **This port:** Pure C for config/daemon + C++ (LLDB SB API) for the bypass loop.  
> **Binary output:** `build/not-amfi` (used by `make boot` to start the daemon before VM launch).

---

## Background

`amfid` is the Apple daemon that enforces code-signing policies on macOS. It is consulted by the kernel whenever a binary is loaded. The class `AMFIPathValidator_macos` owns the final validation decision.

Not-AMFI exploits the fact that LLDB can attach to `amfid` when SIP debugging restrictions are lifted. By installing a software breakpoint on the validator's decision method and patching the CPU return register in-place, it turns a validation failure into a success for specific paths or CDHashes — without modifying any binary on disk or in memory.

---

## How It Works
<img width="2816" height="1536" alt="AMSI Bypass Flowchart" src="https://github.com/user-attachments/assets/b1cc9652-bfd4-49a9-a167-31af4da60b29" />

```
amfid (system daemon)
  │
  └─ -[AMFIPathValidator_macos validateWithError:]
        ▲
        │  LLDB breakpoint (Not-AMFI)
        │
        ├─ 1. Read "self" from x0 (arm64) / rdi (x86_64)
        │      → AMFIPathValidator_macos* pointer
        │
        ├─ 2. StepOutOfFrame()
        │      → execution returns to caller
        │
        ├─ 3. Evaluate ObjC expressions via LLDB expression engine:
        │      (BOOL)   [(id)<ptr> isValid]         → was amfid happy?
        │      (NSURL*) [(id)<ptr> codePath]         → file:///path/to/binary
        │      (NSData*)[(id)<ptr> cdhashAsData]     → <aabb ccdd ...>
        │
        ├─ 4. Match against allow-lists:
        │      • path prefix match   (e.g. /var/jb/)
        │      • exact CDHash match  (20-byte hex)
        │      • --allow-all flag
        │
        └─ 5. If match: patch return register (x0 / rax) ← "1"
               → amfid caller sees: validation PASSED
```

**Config hot-reload:** `~/.not-amfi/paths` and `~/.not-amfi/cdhashes` are watched by mtime. Any edit takes effect on the very next breakpoint hit — no restart needed.

---

## Requirements

| Dependency | Version | Purpose |
|---|---|---|
| macOS | 12+ Monterey | LLDB.framework |
| Xcode | Full install (not CLT-only) | iOS SDK, `codesign`, `xcrun` |
| Homebrew LLVM | `brew install llvm` | LLDB C++ SB API headers (`include/lldb/API/`) + `liblldb.dylib` |
| SIP | Partial or Disabled | `csrutil enable --without debug` + `csrutil allow-research-guests enable` |
| AMFI boot-arg | `amfi_get_out_of_my_way=1` in NVRAM | Permit loading unsigned/ad-hoc-signed code |

### macOS SIP & Recovery Configuration

To allow `not-amfi` to attach to `amfid` via LLDB while preserving system security, configure SIP in Recovery Mode:

1. **Boot into macOS Recovery Mode** (hold Power button on Apple Silicon, or `Cmd+R` on Intel).
2. **Open Terminal** from the Utilities menu.
3. Run the following commands:

```bash
# Disable debug restrictions (allows LLDB to attach to root daemons like amfid)
csrutil enable --without debug

# Enable research guest VMs (required for Virtualization.framework research VMs)
csrutil allow-research-guests enable
```

> **Alternative (Full Disable):**
> ```bash
> csrutil disable
> csrutil allow-research-guests enable
> ```

4. **Restart into macOS**.

> **Why Homebrew LLVM?**  
> Xcode ships `LLDB.framework` without the public C++ SB API headers (`SBDebugger.h`, `SBTarget.h`, etc.) on Apple Silicon macOS. Homebrew LLVM bundles the full header set alongside `liblldb.dylib`, making it the only self-contained option for native compilation.

---

## Build

### Standalone

```bash
# Install LLVM once
brew install llvm

# Build
make                       # → build/not-amfi

# Optional system-wide install
sudo make install          # → /usr/local/bin/not-amfi
```

The `Makefile` auto-detects the prefix via `brew --prefix llvm` — no manual path configuration needed.

### Integration with the parent project

The root `Makefile` owns the `not_amfi_bin` target:

```bash
make not_amfi_bin          # builds C port → build/not-amfi
```

This target is a dependency of `make boot`, so the binary is always up-to-date before the VM starts.

`install.sh` and `scripts/setup_tools.sh` also build Not-AMFI at steps `[6/6]` and `[7/8]` respectively.

---

## Usage

```
Usage: not-amfi [OPTIONS] [COMMAND [ARGS...]]

A simple utility for bypassing amfid signature verification.

Options:
  -p, --path <path>      path of executable to allow
                         (can be specified multiple times;
                          merged with ~/.not-amfi/paths)
  -c, --cdhash <hash>    cdhash of executable to allow
                         (can be specified multiple times;
                          merged with ~/.not-amfi/cdhashes)
  -v, --verbose          enable verbose output
      --allow-all        allow all validations to pass
  -h, --help             show this help and exit

Commands:
  daemon                 start Not-AMFI in daemon mode (detached)
  add-path    <path>     add allowed path prefix to persistent config
  remove-path <path>     remove allowed path prefix from persistent config
  add-cdhash  <hash>     add allowed cdhash to persistent config
  remove-cdhash <hash>   remove allowed cdhash from persistent config

When no COMMAND is given, Not-AMFI runs in foreground bypass mode.
```

### Common usage patterns

```bash
# ── Foreground bypass (blocks until Ctrl-C) ───────────────────────────────
sudo not-amfi --verbose

# ── Allow everything (research/VM use) ────────────────────────────────────
sudo not-amfi --allow-all

# ── Allow a specific app bundle ───────────────────────────────────────────
sudo not-amfi add-path /Users/alice/dev/MyApp.app/
sudo not-amfi --verbose

# ── Allow by CDHash (20-byte hex, no spaces) ──────────────────────────────
sudo not-amfi add-cdhash aabbccddeeff00112233445566778899aabbccdd
sudo not-amfi

# ── One-shot path without persisting ──────────────────────────────────────
sudo not-amfi -p /var/jb/ -p /usr/local/bin/ --verbose

# ── Background daemon mode ────────────────────────────────────────────────
sudo not-amfi daemon --allow-all          # detaches; logs discarded
sudo not-amfi daemon --verbose            # detaches; stderr to /dev/null

# ── Manage the allow-list ─────────────────────────────────────────────────
not-amfi add-path    /var/jb/
not-amfi remove-path /var/jb/
not-amfi add-cdhash    aabb...
not-amfi remove-cdhash aabb...
```

> **Note:** `add-path` / `remove-path` / `add-cdhash` / `remove-cdhash` do **not** require `sudo` — they only write to `~/.not-amfi/`. Only the bypass commands need root to attach to `amfid`.

---

## Persistent Configuration

Allow-lists are stored as plain text files:

| File | Format | Example entry |
|---|---|---|
| `~/.not-amfi/paths` | One path prefix per line | `/var/jb/` |
| `~/.not-amfi/cdhashes` | One 40-char hex string per line | `aabbccddeeff...` |

**Rules:**
- Path matching is **prefix-based** — `/var/jb/` allows any binary under that directory.
- CDHash matching is **case-insensitive exact** — must be the full 20-byte (40 hex char) hash.
- Blank lines and leading/trailing whitespace are ignored.
- CLI `-p`/`-c` flags are merged with the file contents at startup; they are not persisted.
- Files are hot-reloaded on mtime change — **no restart required**.

---

## Daemon Mode

```bash
sudo not-amfi daemon [OPTIONS]
```

Daemon mode forks a child process, calls `setsid()` to detach from the terminal session, and redirects stdio to `/dev/null`. The parent prints the child PID and exits. The child runs the foreground bypass loop indefinitely.

This is used by `make boot` to keep the bypass active while the vphone-cli VM is running:

```bash
# From the root Makefile boot target:
sudo build/not-amfi --allow-all > /tmp/not-amfi.log 2>&1 &
```

---

## Source Layout

```
amfinot_cport/
├── not_amfi.cpp            Main entry point — getopt_long CLI + subcommand dispatch
├── bypass_runtime.h        Public C interface to the LLDB bypass loop
├── bypass_runtime.cpp      LLDB SB API: attach, breakpoint, validate_hook, bypass_loop
├── config_store.h          Public C interface to persistent config
├── config_store.c          ~/.not-amfi/ file I/O, mtime tracking (pure C)
├── daemon_runtime.h        Public C interface to daemon launcher
├── daemon_runtime.c        fork() + setsid() + execv() daemon mode (pure C)
├── Makefile                Builds via Homebrew LLVM (auto-detected prefix)
├── README.md               This file
└── build/
    ├── not-amfi            Compiled binary
    ├── not_amfi.o
    ├── bypass_runtime.o
    ├── config_store.o
    └── daemon_runtime.o
```

### Module responsibilities

| Module | Language | Responsibility |
|---|---|---|
| `not_amfi.cpp` | C++ | `main()`, `getopt_long` arg parsing, subcommand dispatch |
| `bypass_runtime.cpp` | C++ (LLDB SB API) | Attach to `amfid`, set breakpoint, `validate_hook()`, `bypass_loop()` |
| `config_store.c` | C | `~/.not-amfi/` directory, read/write path and cdhash list files, mtime snapshot |
| `daemon_runtime.c` | C | `fork()` + `setsid()` + `execv()` to daemonise the bypass loop |

### Key LLDB SB API types used

| Type | Used for |
|---|---|
| `lldb::SBDebugger` | Top-level LLDB context, `SetAsync(false)` for synchronous operation |
| `lldb::SBTarget` | Empty target, `AttachToProcessWithName()`, `BreakpointCreateByName()`, `EvaluateExpression()` |
| `lldb::SBProcess` | `Continue()`, `GetState()`, `GetNumThreads()`, `Detach()` |
| `lldb::SBThread` | `GetStopReason()`, `GetFrameAtIndex()`, `StepOutOfFrame()` |
| `lldb::SBFrame` | `FindRegister()` — reads `x0`/`rdi` (self) and `x0`/`rax` (return value) |
| `lldb::SBValue` | `GetValue()` (register as string), `SetValueFromCString("1")` (patch return), `GetObjectDescription()` (ObjC -description) |

---

## Security Notes

- Not-AMFI requires root and a relaxed SIP/AMFI configuration (`csrutil enable --without debug` and `csrutil allow-research-guests enable`). **Only use on machines explicitly set up for security research.**
- `--allow-all` disables all AMFI signature checks system-wide for the duration of the session. Scope it with `-p`/`-c` where possible.
- The bypass is purely in-memory and runtime: it patches CPU registers on each breakpoint hit, never modifying files on disk.
- Detaching Not-AMFI (Ctrl-C or process exit) fully restores `amfid` to normal operation.
