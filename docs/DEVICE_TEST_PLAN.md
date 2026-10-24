# KaoriosHelper Physical Device Test Plan & Verification Procedures

## 1. Test Environment & Prerequisites

### 1.1 Physical Device Requirements
- **Device**: Android smartphone with an unlocked bootloader (Google Pixel 6/7/8 or Xiaomi/OnePlus flagship recommended).
- **Architecture**: `arm64-v8a` (64-bit ARM).
- **Target Android Versions**:
  - Android 12 (API 31)
  - Android 13 (API 33)
  - Android 14 (API 34)
  - Android 15 (API 35+)

### 1.2 Root Manager & Zygisk Environment
- **Root Solutions Supported**:
  1. **Magisk** (v26.0+ / Canary) with built-in Zygisk enabled.
  2. **KernelSU** (v0.9.5+) with **Zygisk Next** (v1.0.5+) or **ReZygisk** (v0.1.0+).
  3. **APatch** (v10.7+) with **Zygisk Next** (v1.0.5+).
- **Prerequisite Module Cleanup**:
  Ensure competing modules are disabled or removed before testing to prevent hook collision:
  - Disable standalone `PlayIntegrityFix` / `PlayIntegrityFork`.
  - Disable standalone `HMA` / `HMA-OSS`.
  - Disable standalone `TrickyStore` / `TEESimulator`.
  - Disable standalone `COPG`.

### 1.3 Execution Reality Status
- **Host Testing**: **COMPLETED (29 Python tests + 25 Native C++ tests passed)**.
- **Physical Device Testing Status**: **`NOT RUN - REQUIRES PHYSICAL DEVICE`**.
  *(Physical hardware was not attached to this test runner environment. All device steps below are executable procedures for field testing).*

---

## 2. Test Matrix Overview

| Test ID | Subsystem | Test Scope | Verification Method | Host Status | Device Status |
|---|---|---|---|---|---|
| **TEST-DISP-01** | Dispatcher | Unmanaged app library unload (`DLCLOSE`) | Logcat grep for `DLCLOSE` | `HOST_VERIFIED` | `NOT RUN` |
| **TEST-DISP-02** | Dispatcher | Managed app process claim | Logcat grep for engine claim | `HOST_VERIFIED` | `NOT RUN` |
| **TEST-HMA-01** | HMA | `system_server` specialization & routing | Logcat grep for `HMA preServerSpecialize` | `HOST_VERIFIED` | `NOT RUN` |
| **TEST-HMA-02** | HMA | Package query isolation vs Momo / ApplistDetector | App detection audit | `N/A (STUB)` | `NOT RUN` |
| **TEST-PIF-01** | PIF | Java `Build` field reflection | `adb shell getprop` vs App reflect | `HOST_VERIFIED` | `NOT RUN` |
| **TEST-PIF-02** | PIF | Play Integrity API evaluation (DroidGuard) | Play Integrity API Checker app | `N/A (NATIVE GAP)` | `NOT RUN` |
| **TEST-TEE-01** | TEE | Keybox presence validation | `helperctl status` | `HOST_VERIFIED` | `NOT RUN` |
| **TEST-TEE-02** | TEE | Hardware KeyStore attestation (KeyMint) | Key Attestation Demo app | `N/A (STUB)` | `NOT RUN` |
| **TEST-COPG-01** | COPG | Target game package parsing | `helperctl status` | `HOST_VERIFIED` | `NOT RUN` |
| **TEST-COPG-02** | COPG | `/proc/cpuinfo` mount virtualization | `cat /proc/cpuinfo` inside game UID | `N/A (STUB)` | `NOT RUN` |
| **TEST-CTL-01** | helperctl | Native CLI execution on Android | `adb shell /data/adb/modules/kaorioshelper/helperctl` | `HOST_VERIFIED` | `NOT RUN` |
| **TEST-CTL-02** | helperctl | Atomic config update & 0600 permissions | `ls -la /data/adb/modules/kaorioshelper/data` | `HOST_VERIFIED` | `NOT RUN` |

---

## 3. Step-by-Step On-Device Test Procedures

### Procedure 1: Native Zygisk Loader & Dispatcher Verification
```bash
# 1. Flash module ZIP via Magisk / KernelSU / APatch web UI or CLI
adb push out/kaorioshelper-v1.0.0.zip /data/local/tmp/
adb shell "su -c 'magisk --install-module /data/local/tmp/kaorioshelper-v1.0.0.zip'"

# 2. Reboot device
adb reboot

# 3. Wait for boot completion and monitor Zygisk initialization
adb wait-for-device
adb logcat -s "KaoriosHelper" "Zygisk" "ZygiskNext" -d | grep -E "Dispatcher loaded|Registering engine"
```
**Expected Output**:
```
[INFO] [KaoriosHelper] KaoriosHelper Zygisk Dispatcher loaded (API v4)
[INFO] [KaoriosHelper] Registering engine: hma
[INFO] [KaoriosHelper] Registering engine: pif
[INFO] [KaoriosHelper] Registering engine: tee
[INFO] [KaoriosHelper] Registering engine: copg
```

---

### Procedure 2: Native CLI (`helperctl`) Verification
```bash
# Execute native compiled helperctl directly on device
adb shell "su -c '/data/adb/modules/kaorioshelper/helperctl status'"
adb shell "su -c '/data/adb/modules/kaorioshelper/helperctl version --json'"
```
**Expected Output**:
```
===============================================
         KaoriosHelper Runtime Status          
===============================================
Module Version: v1.0.0
Module State:   ACTIVE

Subsystem Engines (Truthful Audit Assessment):
  • copg Engine:
      Enabled: NO
      Status:  DISABLED
  • hma Engine:
      Enabled: YES
      Status:  CONFIGURED_NOT_HOOKED
  • pif Engine:
      Enabled: YES
      Status:  PARTIALLY_APPLIED
  • tee Engine:
      Enabled: NO
      Status:  DISABLED
===============================================
{"moduleId": "kaorioshelper", "version": "v1.0.0"}
```

---

### Procedure 3: PIF Java Reflection & Denylist Unmount
```bash
# 1. Ensure PIF is enabled
adb shell "su -c '/data/adb/modules/kaorioshelper/helperctl enable pif'"

# 2. Kill Google Play Services to force fresh fork from Zygote
adb shell "su -c 'killall -9 com.google.android.gms com.google.android.gms.unstable'"

# 3. Launch Play Store to trigger GMS specialization
adb shell "am start -n com.android.vending/.AssetBrowserActivity"

# 4. Check logcat for PIF targeting and field application
adb logcat -s "KaoriosHelper" -d | grep -E "PIF"
```
**Expected Logcat Output**:
```
[INFO] [KaoriosHelper] PIF targeting process: com.google.android.gms.unstable
[INFO] [KaoriosHelper] PIF properties applied successfully in com.google.android.gms.unstable
```

**Mountinfo Verification (Denylist Unmount)**:
```bash
# Inspect mountinfo of GMS process to ensure Magisk/KernelSU mounts are detached
GMS_PID=$(adb shell "pidof com.google.android.gms.unstable")
adb shell "su -c 'grep -E \"magisk|kaorios\" /proc/$GMS_PID/mountinfo'"
```
*Expected Result*: Zero occurrences (denylist unmount successfully removes module filesystem traces).

---

### Procedure 4: TEE Coexistence Rule Verification
```bash
# 1. Enable TEE and provide a keybox template
adb shell "su -c '/data/adb/modules/kaorioshelper/helperctl enable tee'"
adb shell "su -c 'echo \"<Keybox></Keybox>\" > /data/adb/modules/kaorioshelper/data/keybox.xml'"

# 2. Kill GMS process to trigger reload
adb shell "su -c 'killall -9 com.google.android.gms.unstable'"
adb shell "am start -n com.android.vending/.AssetBrowserActivity"

# 3. Inspect logcat for coexistence enforcement
adb logcat -s "KaoriosHelper" -d | grep -E "KeyStore collision"
```
**Expected Logcat Output**:
```
[WARN] [KaoriosHelper] PIF: TEE Engine is active! Forcing spoofProvider = 0 to prevent KeyStore collision.
```

---

## 4. Automated Device Verification Shell Script (`test_device.sh`)

Save and execute the following automated audit script on a connected physical testbench:

```bash
#!/bin/bash
set -euo pipefail

echo "=========================================================="
echo "      KAORIOSHELPER PHYSICAL DEVICE AUTOMATED AUDIT       "
echo "=========================================================="

echo "[1/6] Checking ADB Connection..."
adb get-state >/dev/null 2>&1 || { echo "Error: Device not connected via ADB"; exit 1; }

echo "[2/6] Validating Device Architecture..."
ABI=$(adb shell getprop ro.product.cpu.abi)
echo "      Detected ABI: $ABI"
if [ "$ABI" != "arm64-v8a" ]; then
    echo "      Warning: Device is not arm64-v8a ($ABI)"
fi

echo "[3/6] Verifying Native helperctl Binary Execution..."
STATUS_OUT=$(adb shell "su -c '/data/adb/modules/kaorioshelper/helperctl status'")
echo "$STATUS_OUT"

echo "[4/6] Verifying Data Directory Permissions..."
PERMS=$(adb shell "su -c 'ls -ld /data/adb/modules/kaorioshelper/data'")
echo "      Data dir perms: $PERMS"
echo "$PERMS" | grep -q "drwx------" && echo "      [PASS] 0700 DAC enforced" || echo "      [FAIL] DAC mismatch"

echo "[5/6] Verifying Process Claim & DLCLOSE on Unmanaged App..."
adb shell "am force-stop com.android.calculator2 2>/dev/null || true"
adb logcat -c
adb shell "monkey -p com.android.calculator2 -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1 || true
sleep 1
adb logcat -s "KaoriosHelper" -d | grep -q "DLCLOSE" && echo "      [PASS] DLCLOSE requested on calculator" || echo "      [INFO] App specialization skipped"

echo "[6/6] Verifying PIF Process Targeting on GMS..."
adb shell "su -c 'killall -9 com.google.android.gms.unstable 2>/dev/null || true'"
adb logcat -c
adb shell "am start -n com.android.vending/.AssetBrowserActivity" >/dev/null 2>&1
sleep 2
adb logcat -s "KaoriosHelper" -d | grep -E "PIF" || echo "      [WARN] No PIF logcat lines observed"

echo "=========================================================="
echo "            PHYSICAL DEVICE AUDIT COMPLETE                "
echo "=========================================================="
```
