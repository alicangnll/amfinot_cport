# =============================================================================
# amfidont — C/C++ port Makefile
# =============================================================================
#
# Requirements:
#   - macOS 12+ (LLDB.framework via Xcode)
#   - Xcode (or Xcode Command Line Tools with a full Xcode install for LLDB.framework)
#   - Run as root for amfid attach
#
# Usage:
#   make            Build the amfidont binary
#   make clean      Remove build artefacts
#   make install    Copy binary to /usr/local/bin/amfidont
#
# =============================================================================

# --------------------------------------------------------------------------- #
# Toolchain
# --------------------------------------------------------------------------- #

CC   := clang
CXX  := clang++
LD   := clang++

# --------------------------------------------------------------------------- #
# LLDB — Homebrew LLVM (ships with C++ SB API headers + liblldb.dylib)
# --------------------------------------------------------------------------- #
#
# Xcode's LLDB.framework does not ship public C++ headers on this system.
# Homebrew LLVM provides both:
#   include/lldb/API/   — all SB* headers
#   lib/liblldb.dylib   — the shared library
#
# Install: brew install llvm

LLVM_PREFIX := $(shell brew --prefix llvm 2>/dev/null)

ifeq ($(LLVM_PREFIX),)
$(error Homebrew LLVM not found. Run: brew install llvm)
endif

LLDB_HEADERS := $(LLVM_PREFIX)/include
LLDB_LIB_DIR := $(LLVM_PREFIX)/lib
LLDB_RPATH   := $(LLDB_LIB_DIR)

# --------------------------------------------------------------------------- #
# Flags
# --------------------------------------------------------------------------- #

CFLAGS := \
    -std=c17 \
    -Wall \
    -Wextra \
    -O2 \
    -I.

CXXFLAGS := \
    -std=c++17 \
    -Wall \
    -Wextra \
    -O2 \
    -I. \
    -I$(LLDB_HEADERS)

LDFLAGS := \
    -L$(LLDB_LIB_DIR) \
    -llldb \
    -Wl,-rpath,$(LLDB_RPATH)

# --------------------------------------------------------------------------- #
# Sources and objects
# --------------------------------------------------------------------------- #

BUILD_DIR := build

C_SRCS := \
    config_store.c \
    daemon_runtime.c

CXX_SRCS := \
    bypass_runtime.cpp \
    amfidont.cpp

C_OBJS   := $(patsubst %.c,   $(BUILD_DIR)/%.o, $(C_SRCS))
CXX_OBJS := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(CXX_SRCS))
ALL_OBJS := $(C_OBJS) $(CXX_OBJS)

TARGET := $(BUILD_DIR)/amfidont

# --------------------------------------------------------------------------- #
# Targets
# --------------------------------------------------------------------------- #

.PHONY: all clean install info

all: info $(TARGET)

info:
	@echo "LLVM prefix  : $(LLVM_PREFIX)"
	@echo "LLDB headers : $(LLDB_HEADERS)"
	@echo "LLDB lib dir : $(LLDB_LIB_DIR)"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Compile C sources
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile C++ sources
$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Link
$(TARGET): $(ALL_OBJS)
	$(LD) $(ALL_OBJS) $(LDFLAGS) -o $@
	@echo "Built: $@"

clean:
	rm -rf $(BUILD_DIR)

install: all
	install -m 755 $(TARGET) /usr/local/bin/amfidont
	@echo "Installed to /usr/local/bin/amfidont"
