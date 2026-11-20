#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# KaoriosHelper Android Rust Cross-Compilation Pipeline (Multi-ABI: arm64-v8a, armeabi-v7a)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if ! command -v cargo >/dev/null 2>&1; then
    echo "ERROR: cargo not found. Install Rust toolchain with aarch64-linux-android and armv7-linux-androideabi targets." >&2
    exit 1
fi

# Discover Android NDK
NDK=""
for candidate in \
    "${ANDROID_NDK_HOME:-}" \
    "${ANDROID_NDK_ROOT:-}" \
    "${ANDROID_NDK_LATEST_HOME:-}" \
    "/home/hzzmonet/toolchains/android-ndk-r26b" \
    "${ANDROID_HOME:-/opt/android-sdk}/ndk-bundle" \
    $(ls -d "${ANDROID_HOME:-/opt/android-sdk}/ndk/"* 2>/dev/null | sort -V | tail -n 1 || true); do
    if [ -n "${candidate}" ] && [ -d "${candidate}/toolchains/llvm/prebuilt/linux-x86_64/bin" ]; then
        NDK="${candidate}"
        break
    fi
done

if [ -z "${NDK}" ]; then
    echo "ERROR: Android NDK toolchain not found. Set ANDROID_NDK_HOME." >&2
    exit 1
fi

if ! grep -qE '^Pkg.Revision[[:space:]]*=[[:space:]]*26\.1\.10909125[[:space:]]*$' "${NDK}/source.properties"; then
    printf '%s\n' 'ERROR: Android NDK r26b (26.1.10909125) is required.' >&2
    exit 1
fi

LLVM_BIN="${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin"
echo "Using Android NDK at: ${NDK}"
echo "LLVM Toolchain: ${LLVM_BIN}"

API_LEVEL=31
BUILD_TARGET="${1:-all}"

OUT_MODULE_DIR="${ROOT_DIR}/module"
ZYGISK_DIR="${OUT_MODULE_DIR}/zygisk"
mkdir -p "${ZYGISK_DIR}" "${OUT_MODULE_DIR}/bin" "${ROOT_DIR}/out"

VERSION_SCRIPT="${ROOT_DIR}/scripts/zygisk_export.map"
cat << 'EOF' > "${VERSION_SCRIPT}"
{
    global:
        zygisk_module_entry;
        zygisk_companion_entry;
    local:
        *;
};
EOF

verify_shared_library() {
    local so_file="$1"
    local expected_machine="$2"
    local expected_soname="$3"
    local readelf_bin="$4"

    echo "Verifying ELF headers and exports for ${so_file}..."
    local machine=$("${readelf_bin}" -h "${so_file}" | grep "Machine:" || true)
    echo "  ${machine}"
    if ! echo "${machine}" | grep -qi "${expected_machine}"; then
        echo "ERROR: ${so_file} is not valid ${expected_machine} ELF!" >&2
        exit 1
    fi

    local soname=$("${readelf_bin}" -d "${so_file}" | grep "SONAME" || true)
    echo "  ${soname}"
    if ! echo "${soname}" | grep -q "${expected_soname}"; then
        echo "ERROR: ${so_file} SONAME mismatch (expected ${expected_soname})!" >&2
        exit 1
    fi

    local exports=$("${readelf_bin}" --dyn-syms "${so_file}" | grep -E "zygisk_module_entry|zygisk_companion_entry" || true)
    echo "  Exported entrypoints: $(echo "${exports}" | tr '\n' ' ')"
    if ! echo "${exports}" | grep -q "zygisk_module_entry"; then
        echo "ERROR: zygisk_module_entry missing from dynamic symbol table of ${so_file}!" >&2
        exit 1
    fi
    if ! echo "${exports}" | grep -q "zygisk_companion_entry"; then
        echo "ERROR: zygisk_companion_entry missing from dynamic symbol table of ${so_file}!" >&2
        exit 1
    fi

    echo "✓ ${so_file} ELF verification passed!"
}

build_arm64() {
    echo "=========================================================="
    echo "  Building KaoriosHelper (Rust) for arm64-v8a (API ${API_LEVEL})"
    echo "=========================================================="

    local CC="${LLVM_BIN}/aarch64-linux-android${API_LEVEL}-clang"
    local AR="${LLVM_BIN}/llvm-ar"
    local STRIP="${LLVM_BIN}/llvm-strip"
    local READELF="${LLVM_BIN}/llvm-readelf"

    CC_aarch64_linux_android="${CC}" \
    AR_aarch64_linux_android="${AR}" \
    CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="${CC}" \
    RUSTFLAGS="-C link-arg=-Wl,-z,max-page-size=16384" \
    cargo build --manifest-path "${ROOT_DIR}/Cargo.toml" \
        --release --target aarch64-linux-android \
        -p kaorios_zygisk -p helperctl -p kaorios_fixture

    local TARGET_DIR="${ROOT_DIR}/target/aarch64-linux-android/release"
    local TARGET_SO="${ZYGISK_DIR}/arm64-v8a.so"
    cp -f "${TARGET_DIR}/libkaorioshelper.so" "${TARGET_SO}"
    "${STRIP}" --strip-unneeded "${TARGET_SO}"

    local TARGET_CTL="${OUT_MODULE_DIR}/helperctl"
    local TARGET_CTL_ARM64="${OUT_MODULE_DIR}/bin/arm64-v8a/helperctl"
    mkdir -p "${OUT_MODULE_DIR}/bin/arm64-v8a"
    cp -f "${TARGET_DIR}/helperctl" "${TARGET_CTL}"
    "${STRIP}" --strip-unneeded "${TARGET_CTL}"
    chmod 0755 "${TARGET_CTL}"
    cp -f "${TARGET_CTL}" "${TARGET_CTL_ARM64}"
    chmod 0755 "${TARGET_CTL_ARM64}"

    local TARGET_FIXTURE="${ROOT_DIR}/out/kaorios_fixture"
    cp -f "${TARGET_DIR}/kaorios_fixture" "${TARGET_FIXTURE}"
    "${STRIP}" --strip-unneeded "${TARGET_FIXTURE}"
    chmod 0755 "${TARGET_FIXTURE}"

    verify_shared_library "${TARGET_SO}" "AArch64" "arm64-v8a.so" "${READELF}"
    echo "✓ arm64-v8a Rust build verified successfully!"
}

build_armeabi_v7a() {
    echo "=========================================================="
    echo "  Building KaoriosHelper (Rust) for armeabi-v7a (API ${API_LEVEL})"
    echo "=========================================================="

    local CC="${LLVM_BIN}/armv7a-linux-androideabi${API_LEVEL}-clang"
    local AR="${LLVM_BIN}/llvm-ar"
    local STRIP="${LLVM_BIN}/llvm-strip"
    local READELF="${LLVM_BIN}/llvm-readelf"

    CC_armv7_linux_androideabi="${CC}" \
    AR_armv7_linux_androideabi="${AR}" \
    CARGO_TARGET_ARMV7_LINUX_ANDROIDEABI_LINKER="${CC}" \
    cargo build --manifest-path "${ROOT_DIR}/Cargo.toml" \
        --release --target armv7-linux-androideabi \
        -p kaorios_zygisk -p helperctl

    local TARGET_DIR="${ROOT_DIR}/target/armv7-linux-androideabi/release"
    local TARGET_SO="${ZYGISK_DIR}/armeabi-v7a.so"
    cp -f "${TARGET_DIR}/libkaorioshelper.so" "${TARGET_SO}"
    "${STRIP}" --strip-unneeded "${TARGET_SO}"

    local TARGET_CTL="${OUT_MODULE_DIR}/bin/armeabi-v7a/helperctl"
    mkdir -p "${OUT_MODULE_DIR}/bin/armeabi-v7a"
    cp -f "${TARGET_DIR}/helperctl" "${TARGET_CTL}"
    "${STRIP}" --strip-unneeded "${TARGET_CTL}"
    chmod 0755 "${TARGET_CTL}"

    verify_shared_library "${TARGET_SO}" "ARM" "armeabi-v7a.so" "${READELF}"
    echo "✓ armeabi-v7a Rust build verified successfully!"
}

case "${BUILD_TARGET}" in
    arm64|arm64-v8a)
        build_arm64
        ;;
    armeabi-v7a|arm)
        build_armeabi_v7a
        ;;
    all)
        build_arm64
        build_armeabi_v7a
        ;;
    *)
        echo "Usage: $0 [arm64-v8a|armeabi-v7a|all]"
        exit 1
        ;;
esac

echo "=========================================================="
echo "  Cross-Compilation Complete! Output files in module/"
echo "=========================================================="
ls -lh "${ZYGISK_DIR}"/*.so "${OUT_MODULE_DIR}/bin/"*/helperctl
