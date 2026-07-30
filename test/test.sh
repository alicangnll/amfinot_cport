#!/usr/bin/env zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "${SCRIPT_DIR}"

TEST_BINARY="./amfi_test"
ENTITLEMENTS="./entitlements.plist"

echo "============================================================"
echo " AMFI Bypass Verification & Comparison Test Tool"
echo "============================================================"

# 1. Daemon Status Check
DAEMON_PID=$(pgrep -x "not-amfi" || pgrep -f "build/not-amfi" | head -n 1 || true)

echo "[+] Detecting Not-AMFI Daemon Status..."
if [ -n "${DAEMON_PID}" ]; then
    echo "    -> Not-AMFI Daemon IS RUNNING (PID: ${DAEMON_PID})"
else
    echo "    -> Not-AMFI Daemon is NOT running."
fi
echo ""

# 2. Expected Behaviors Matrix
echo "------------------------------------------------------------"
echo " Expected Behaviors Matrix:"
echo " 1. Not-AMFI OFF     (AMFI Active):"
echo "    - Ad-hoc binary carrying restricted private entitlement is blocked."
echo "    - Kernel kills process instantly (Killed: 9 / Signal 9 / Exit 137)."
echo " 2. Not-AMFI ON      (Bypass Active):"
echo "    - not-amfi hooks amfid validation."
echo "    - Binary executes successfully (Exit code 42 / SUCCESS)."
echo "------------------------------------------------------------"
echo ""

# 3. Create entitlements.plist if missing
if [ ! -f "${ENTITLEMENTS}" ]; then
    echo "[1/3] Creating entitlements.plist..."
    cat << 'EOF' > "${ENTITLEMENTS}"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.private.hypervisor</key>
    <true/>
</dict>
</plist>
EOF
fi

# 4. Build dummy C test binary
echo "[2/3] Building dummy C test binary..."
clang -x c - -o "${TEST_BINARY}" << 'EOF'
#include <stdio.h>

int main(void) {
    printf("SUCCESS: Executed process with ad-hoc signed private entitlement!\n");
    return 42;
}
EOF

# 5. Ad-hoc sign binary with restricted entitlements
echo "[3/3] Ad-hoc signing binary with private entitlement (com.apple.private.hypervisor)..."
codesign -s - --entitlements "${ENTITLEMENTS}" --force "${TEST_BINARY}" > /dev/null 2>&1

echo ""
echo "[*] RUNNING LIVE TEST..."
echo "------------------------------------------------------------"

set +e
"${TEST_BINARY}"
EXIT_CODE=$?
set -e

echo "------------------------------------------------------------"

if [ ${EXIT_CODE} -eq 42 ]; then
    echo " [✓] STATUS: Not-AMFI ON / BYPASS ACTIVE!"
    echo "     Privileged ad-hoc binary executed successfully without being blocked."
    rm -f "${TEST_BINARY}"
    exit 0
else
    echo " [X] STATUS: Not-AMFI OFF / AMFI BLOCKED!"
    echo "     Process was killed by AMFI (Exit status: ${EXIT_CODE})."
    echo "     To start Not-AMFI daemon: sudo ./build/not-amfi daemon"
    rm -f "${TEST_BINARY}"
    exit 1
fi
