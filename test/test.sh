#!/usr/bin/env zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_BINARY="${SCRIPT_DIR}/amfi_test"
ENTITLEMENTS="${SCRIPT_DIR}/entitlements.plist"

echo "============================================================"
echo " AMFI Bypass Verification & Comparison Test Tool"
echo "============================================================"

# 1. Daemon Status Check
DAEMON_PID=$(pgrep -x "amfidont" || pgrep -f "build/amfidont" | head -n 1 || true)

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
echo " 1. Not-AMFI KAPALI  (AMFI Aktif):"
echo "    - Kısıtlı private entitlement taşıyan ad-hoc binary engellenir."
echo "    - Kernel süreci anında öldürür (Killed: 9 / Signal 9 / Exit 137)."
echo " 2. Not-AMFI AÇIK    (Bypass Aktif):"
echo "    - amfidont, amfid doğrulamasını hook'lar."
echo "    - Binary başarıyla çalışır (Exit code 42 / SUCCESS)."
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
codesign -s - --entitlements "${ENTITLEMENTS}" --force "${TEST_BINARY}"

echo ""
echo "[*] RUNNING LIVE TEST..."
echo "------------------------------------------------------------"

set +e
"${TEST_BINARY}"
EXIT_CODE=$?
set -e

echo "------------------------------------------------------------"

if [ ${EXIT_CODE} -eq 42 ]; then
    echo " [✓] DURUM: Not-AMFI AÇIK / BYPASS AKTİF!"
    echo "     Özel yetkili ad-hoc binary engellenmeden başarıyla çalıştı."
    rm -f "${TEST_BINARY}"
    exit 0
else
    echo " [X] DURUM: Not-AMFI KAPALI / AMFI ENGELLEDİ!"
    echo "     Süreç AMFI tarafından öldürüldü (Exit status: ${EXIT_CODE})."
    echo "     Not-AMFI daemon'ını başlatmak için: sudo ./build/amfidont daemon"
    rm -f "${TEST_BINARY}"
    exit 1
fi
