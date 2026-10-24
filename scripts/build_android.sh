#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# KaoriosHelper Android NDK Cross-Compilation Pipeline

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-/home/hzzmonet/toolchains/android-ndk-r26b}"
LLVM_BIN="${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin"

if [ ! -d "${LLVM_BIN}" ]; then
    echo "ERROR: Android NDK toolchain not found at ${LLVM_BIN}"
    exit 1
fi

API_LEVEL=31
BUILD_TARGET="${1:-all}"

OUT_MODULE_DIR="${ROOT_DIR}/module"
ZYGISK_DIR="${OUT_MODULE_DIR}/zygisk"
mkdir -p "${ZYGISK_DIR}"

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

build_arm64() {
    echo "=========================================================="
    echo "  Building KaoriosHelper for arm64-v8a (Android API ${API_LEVEL})"
    echo "=========================================================="

    local CC="${LLVM_BIN}/aarch64-linux-android${API_LEVEL}-clang"
    local CXX="${LLVM_BIN}/aarch64-linux-android${API_LEVEL}-clang++"
    local STRIP="${LLVM_BIN}/llvm-strip"
    local READELF="${LLVM_BIN}/llvm-readelf"

    local BUILD_DIR="/tmp/kaorios_build_arm64"
    rm -rf "${BUILD_DIR}"
    mkdir -p "${BUILD_DIR}"

    local INCLUDES=(
        "-I${ROOT_DIR}/third_party/zygisk"
        "-I${ROOT_DIR}/zygisk/include"
        "-I${ROOT_DIR}/engines/hma"
        "-I${ROOT_DIR}/engines/pif"
        "-I${ROOT_DIR}/engines/tee"
        "-I${ROOT_DIR}/engines/copg"
        "-I${ROOT_DIR}/third_party/shadowhook/include"
        "-I${ROOT_DIR}/third_party/shadowhook"
        "-I${ROOT_DIR}/third_party/shadowhook/common"
        "-I${ROOT_DIR}/third_party/shadowhook/arch/arm64"
        "-I${ROOT_DIR}/third_party/shadowhook/third_party/xdl"
        "-I${ROOT_DIR}/third_party/shadowhook/third_party/bsd"
        "-I${ROOT_DIR}/third_party/shadowhook/third_party/lss"
    )

    local SH_SRCS=(
        "${ROOT_DIR}/third_party/shadowhook/shadowhook.c"
        "${ROOT_DIR}/third_party/shadowhook/sh_elf.c"
        "${ROOT_DIR}/third_party/shadowhook/sh_enter.c"
        "${ROOT_DIR}/third_party/shadowhook/sh_hub.c"
        "${ROOT_DIR}/third_party/shadowhook/sh_island.c"
        "${ROOT_DIR}/third_party/shadowhook/sh_jni.c"
        "${ROOT_DIR}/third_party/shadowhook/sh_linker.c"
        "${ROOT_DIR}/third_party/shadowhook/sh_recorder.c"
        "${ROOT_DIR}/third_party/shadowhook/sh_safe.c"
        "${ROOT_DIR}/third_party/shadowhook/sh_switch.c"
        "${ROOT_DIR}/third_party/shadowhook/sh_task.c"
        "${ROOT_DIR}/third_party/shadowhook/sh_xdl.c"
        "${ROOT_DIR}/third_party/shadowhook/common/bytesig.c"
        "${ROOT_DIR}/third_party/shadowhook/common/sh_errno.c"
        "${ROOT_DIR}/third_party/shadowhook/common/sh_log.c"
        "${ROOT_DIR}/third_party/shadowhook/common/sh_ref.c"
        "${ROOT_DIR}/third_party/shadowhook/common/sh_trampo.c"
        "${ROOT_DIR}/third_party/shadowhook/common/sh_util.c"
        "${ROOT_DIR}/third_party/shadowhook/arch/arm64/sh_a64.c"
        "${ROOT_DIR}/third_party/shadowhook/arch/arm64/sh_inst.c"
        "${ROOT_DIR}/third_party/shadowhook/arch/arm64/sh_glue.S"
        "${ROOT_DIR}/third_party/shadowhook/third_party/xdl/xdl.c"
        "${ROOT_DIR}/third_party/shadowhook/third_party/xdl/xdl_iterate.c"
        "${ROOT_DIR}/third_party/shadowhook/third_party/xdl/xdl_linker.c"
        "${ROOT_DIR}/third_party/shadowhook/third_party/xdl/xdl_lzma.c"
        "${ROOT_DIR}/third_party/shadowhook/third_party/xdl/xdl_util.c"
    )

    echo "Compiling ShadowHook Bionic hook engine (AArch64)..."
    for src in "${SH_SRCS[@]}"; do
        local obj="${BUILD_DIR}/$(basename "${src}").o"
        "${CC}" -O3 -fPIC -fvisibility=hidden "${INCLUDES[@]}" -c "${src}" -o "${obj}"
    done

    local CORE_SRCS=(
        "${ROOT_DIR}/zygisk/src/dispatcher.cpp"
        "${ROOT_DIR}/engines/hma/hma_engine.cpp"
        "${ROOT_DIR}/engines/pif/pif_engine.cpp"
        "${ROOT_DIR}/engines/tee/tee_engine.cpp"
        "${ROOT_DIR}/engines/copg/copg_engine.cpp"
        "${ROOT_DIR}/zygisk/src/entry.cpp"
    )

    echo "Compiling Unified Engine Runtime (C++20)..."
    for src in "${CORE_SRCS[@]}"; do
        local obj="${BUILD_DIR}/$(basename "${src}").o"
        "${CXX}" -std=c++20 -O3 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden "${INCLUDES[@]}" -c "${src}" -o "${obj}"
    done

    local TARGET_SO="${ZYGISK_DIR}/arm64-v8a.so"
    echo "Linking ${TARGET_SO} (16KB page-aligned, Bionic libc)..."
    "${CXX}" -shared -O3 -static-libstdc++ \
        -Wl,-z,max-page-size=16384 \
        -Wl,--version-script="${VERSION_SCRIPT}" \
        -Wl,--gc-sections \
        "${BUILD_DIR}"/*.o \
        -llog \
        -o "${TARGET_SO}"

    "${STRIP}" --strip-unneeded "${TARGET_SO}"

    # Build native helperctl CLI binary for Android ARM64
    local TARGET_CTL="${OUT_MODULE_DIR}/helperctl"
    local TARGET_CTL_ARM64="${OUT_MODULE_DIR}/bin/arm64-v8a/helperctl"
    mkdir -p "${OUT_MODULE_DIR}/bin/arm64-v8a"
    echo "Compiling native helperctl CLI for Android ARM64..."
    "${CXX}" -std=c++20 -O3 -static-libstdc++ -pie -fPIE \
        -Wl,-z,max-page-size=16384 \
        "${ROOT_DIR}/cli/helperctl/helperctl.cpp" \
        -o "${TARGET_CTL}"
    "${STRIP}" --strip-unneeded "${TARGET_CTL}"
    chmod 0755 "${TARGET_CTL}"
    cp -f "${TARGET_CTL}" "${TARGET_CTL_ARM64}"
    chmod 0755 "${TARGET_CTL_ARM64}"

    # Build diagnostic test fixture for Android ARM64
    local TARGET_FIXTURE="${ROOT_DIR}/out/kaorios_fixture"
    mkdir -p "${ROOT_DIR}/out"
    echo "Compiling diagnostic test fixture for Android ARM64..."
    "${CXX}" -std=c++20 -O3 -static-libstdc++ -pie -fPIE \
        -Wl,-z,max-page-size=16384 \
        "${ROOT_DIR}/tools/test_fixture/fixture_main.cpp" \
        -o "${TARGET_FIXTURE}"
    "${STRIP}" --strip-unneeded "${TARGET_FIXTURE}"
    chmod 0755 "${TARGET_FIXTURE}"

    echo "Verifying arm64-v8a binary ELF header..."
    local machine=$("${READELF}" -h "${TARGET_SO}" | grep "Machine:" || true)
    echo "  ${machine}"
    if ! echo "${machine}" | grep -q "AArch64"; then
        echo "ERROR: ${TARGET_SO} is not valid AArch64 ELF!"
        exit 1
    fi

    echo "Verifying exported symbols (must export only Zygisk entrypoints)..."
    local exports=$("${READELF}" --dyn-syms "${TARGET_SO}" | grep -E "zygisk_module_entry|zygisk_companion_entry" || true)
    echo "${exports}"
    if ! echo "${exports}" | grep -q "zygisk_module_entry"; then
        echo "ERROR: zygisk_module_entry missing from dynamic symbol table!"
        exit 1
    fi

    echo "✓ arm64-v8a build verified successfully!"
}

build_x86_64() {
    echo "=========================================================="
    echo "  Building KaoriosHelper for x86_64 (Android API ${API_LEVEL})"
    echo "=========================================================="

    local CC="${LLVM_BIN}/x86_64-linux-android${API_LEVEL}-clang"
    local CXX="${LLVM_BIN}/x86_64-linux-android${API_LEVEL}-clang++"
    local STRIP="${LLVM_BIN}/llvm-strip"
    local READELF="${LLVM_BIN}/llvm-readelf"

    local BUILD_DIR="/tmp/kaorios_build_x86_64"
    rm -rf "${BUILD_DIR}"
    mkdir -p "${BUILD_DIR}"

    local INCLUDES=(
        "-I${ROOT_DIR}/third_party/zygisk"
        "-I${ROOT_DIR}/zygisk/include"
        "-I${ROOT_DIR}/engines/hma"
        "-I${ROOT_DIR}/engines/pif"
        "-I${ROOT_DIR}/engines/tee"
        "-I${ROOT_DIR}/engines/copg"
        "-I${ROOT_DIR}/third_party/shadowhook/include"
        "-I${ROOT_DIR}/third_party/shadowhook"
        "-I${ROOT_DIR}/third_party/shadowhook/common"
        "-I${ROOT_DIR}/third_party/shadowhook/third_party/xdl"
        "-I${ROOT_DIR}/third_party/shadowhook/third_party/bsd"
        "-I${ROOT_DIR}/third_party/shadowhook/third_party/lss"
    )

    local SH_SRCS=(
        "${ROOT_DIR}/third_party/shadowhook/arch/x86_64/shadowhook_x86_64_stub.c"
    )

    for src in "${SH_SRCS[@]}"; do
        local obj="${BUILD_DIR}/$(basename "${src}").o"
        "${CC}" -O3 -fPIC -fvisibility=hidden "${INCLUDES[@]}" -c "${src}" -o "${obj}"
    done

    local CORE_SRCS=(
        "${ROOT_DIR}/zygisk/src/dispatcher.cpp"
        "${ROOT_DIR}/engines/hma/hma_engine.cpp"
        "${ROOT_DIR}/engines/pif/pif_engine.cpp"
        "${ROOT_DIR}/engines/tee/tee_engine.cpp"
        "${ROOT_DIR}/engines/copg/copg_engine.cpp"
        "${ROOT_DIR}/zygisk/src/entry.cpp"
    )

    for src in "${CORE_SRCS[@]}"; do
        local obj="${BUILD_DIR}/$(basename "${src}").o"
        "${CXX}" -std=c++20 -O3 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden "${INCLUDES[@]}" -c "${src}" -o "${obj}"
    done

    local TARGET_SO="${ZYGISK_DIR}/x86_64.so"
    echo "Linking ${TARGET_SO} (Bionic libc)..."
    "${CXX}" -shared -O3 -static-libstdc++ \
        -Wl,-z,max-page-size=16384 \
        -Wl,--version-script="${VERSION_SCRIPT}" \
        -Wl,--gc-sections \
        "${BUILD_DIR}"/*.o \
        -llog \
        -o "${TARGET_SO}"

    "${STRIP}" --strip-unneeded "${TARGET_SO}"

    # Build native helperctl CLI binary for Android x86_64
    local TARGET_CTL="${OUT_MODULE_DIR}/bin/x86_64/helperctl"
    mkdir -p "${OUT_MODULE_DIR}/bin/x86_64"
    echo "Compiling native helperctl CLI for Android x86_64..."
    "${CXX}" -std=c++20 -O3 -static-libstdc++ -pie -fPIE \
        -Wl,-z,max-page-size=16384 \
        "${ROOT_DIR}/cli/helperctl/helperctl.cpp" \
        -o "${TARGET_CTL}"
    "${STRIP}" --strip-unneeded "${TARGET_CTL}"
    chmod 0755 "${TARGET_CTL}"

    # Build diagnostic test fixture for Android x86_64
    local TARGET_FIXTURE="${ROOT_DIR}/out/kaorios_fixture_x86_64"
    mkdir -p "${ROOT_DIR}/out"
    echo "Compiling diagnostic test fixture for Android x86_64..."
    "${CXX}" -std=c++20 -O3 -static-libstdc++ -pie -fPIE \
        -Wl,-z,max-page-size=16384 \
        "${ROOT_DIR}/tools/test_fixture/fixture_main.cpp" \
        -o "${TARGET_FIXTURE}"
    "${STRIP}" --strip-unneeded "${TARGET_FIXTURE}"
    chmod 0755 "${TARGET_FIXTURE}"

    echo "Verifying x86_64 binary ELF header..."
    local machine=$("${READELF}" -h "${TARGET_SO}" | grep "Machine:" || true)
    echo "  ${machine}"
    if ! echo "${machine}" | grep -q "Advanced Micro Devices X86-64"; then
        echo "ERROR: ${TARGET_SO} is not valid x86_64 ELF!"
        exit 1
    fi

    local ctl_machine=$("${READELF}" -h "${TARGET_CTL}" | grep "Machine:" || true)
    echo "  helperctl (x86_64): ${ctl_machine}"
    if ! echo "${ctl_machine}" | grep -q "Advanced Micro Devices X86-64"; then
        echo "ERROR: ${TARGET_CTL} is not valid x86_64 ELF!"
        exit 1
    fi

    echo "✓ x86_64 build verified successfully!"
}

case "${BUILD_TARGET}" in
    arm64|arm64-v8a)
        build_arm64
        ;;
    x86_64)
        build_x86_64
        ;;
    all)
        build_arm64
        build_x86_64
        ;;
    *)
        echo "Usage: $0 [arm64-v8a|x86_64|all]"
        exit 1
        ;;
esac

echo "=========================================================="
echo "  Cross-Compilation Complete! Output files in module/"
echo "=========================================================="
ls -lh "${ZYGISK_DIR}"/*.so "${OUT_MODULE_DIR}/helperctl"
