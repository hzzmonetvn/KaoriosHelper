#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# On-Device Verification & Log Collection Script for KaoriosHelper (Rust)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ZIP_PATH="${1:-${ROOT_DIR}/out/kaorioshelper-v1.0.0.zip}"
MODE="${2:---verify-only}" # --install | --verify-only

if ! command -v adb >/dev/null 2>&1; then
    echo "ERROR: adb not found in PATH." >&2
    exit 1
fi

echo "[1/5] Checking ADB device connection..."
adb wait-for-device
ABI="$(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
REL="$(adb shell getprop ro.build.version.release | tr -d '\r')"
SDK="$(adb shell getprop ro.build.version.sdk | tr -d '\r')"
MODEL="$(adb shell getprop ro.product.model | tr -d '\r')"
echo "  Device: ${MODEL} (Android ${REL}, API ${SDK}, ABI ${ABI})"

if [ "${MODE}" = "--install" ]; then
    if [ ! -f "${ZIP_PATH}" ]; then
        echo "ERROR: Module ZIP not found at ${ZIP_PATH}" >&2
        exit 1
    fi
    echo "[2/5] Pushing and installing ${ZIP_PATH}..."
    adb push "${ZIP_PATH}" /data/local/tmp/kaorioshelper.zip
    adb shell "su -c '
        if command -v magisk >/dev/null 2>&1; then
            magisk --install-module /data/local/tmp/kaorioshelper.zip
        elif command -v ksud >/dev/null 2>&1; then
            ksud module install /data/local/tmp/kaorioshelper.zip
        elif command -v apd >/dev/null 2>&1; then
            apd module install /data/local/tmp/kaorioshelper.zip
        else
            echo \"ERROR: No supported root manager CLI (magisk/ksud/apd) found\" >&2
            exit 1
        fi
    '"
    echo "  Module installed. Reboot the device now, then re-run:"
    echo "    bash scripts/device_test.sh \"${ZIP_PATH}\" --verify-only"
    exit 0
else
    echo "[2/5] Skipping install (--verify-only). Checking installed module..."
fi

echo "[3/5] Inspecting /data/adb/modules/kaorioshelper..."
adb shell "su -c 'ls -la /data/adb/modules/kaorioshelper /data/adb/modules/kaorioshelper/zygisk /data/adb/modules/kaorioshelper/data'"

echo "[4/5] Running helperctl version, status, diagnostics..."
mkdir -p "${ROOT_DIR}/out"
adb shell "su -c '/data/adb/modules/kaorioshelper/helperctl version --json'" | tee "${ROOT_DIR}/out/device_version.json"
adb shell "su -c '/data/adb/modules/kaorioshelper/helperctl status --json'" | tee "${ROOT_DIR}/out/device_status.json"
adb shell "su -c '/data/adb/modules/kaorioshelper/helperctl diagnostics'" | tee "${ROOT_DIR}/out/device_diagnostics.txt"

if [ -f "${ROOT_DIR}/out/kaorios_fixture" ]; then
    echo "Running kaorios_fixture on device..."
    adb push "${ROOT_DIR}/out/kaorios_fixture" /data/local/tmp/kaorios_fixture >/dev/null
    adb shell "chmod 0755 /data/local/tmp/kaorios_fixture && /data/local/tmp/kaorios_fixture" | tee "${ROOT_DIR}/out/device_fixture.json"
fi

echo "[5/5] Capturing Zygisk / KaoriosHelper logcat..."
adb logcat -d -s KaoriosHelper Zygisk ZygiskNext ReZygisk > "${ROOT_DIR}/out/device_logcat.txt" || true
echo "Saved device logs & reports to out/device_*.{txt,json}"
