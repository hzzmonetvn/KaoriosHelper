#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# KaoriosHelper Android NDK Cross-Compilation Pipeline (Multi-ABI: arm64, x86_64, arm, x86)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

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

LLVM_BIN="${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin"
echo "Using Android NDK at: ${NDK}"
echo "LLVM Toolchain: ${LLVM_BIN}"

API_LEVEL=31
BUILD_TARGET="${1:-all}"

OUT_MODULE_DIR="${ROOT_DIR}/module"
ZYGISK_DIR="${OUT_MODULE_DIR}/zygisk"
mkdir -p "${ZYGISK_DIR}" "${OUT_MODULE_DIR}/bin"

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

    # Verify no forbidden internal symbols leaked into dynamic symbol table
    local leaked=$("${readelf_bin}" --dyn-syms "${so_file}" | grep -E "s_active_spoof_props|my_system_property_read_callback|g_callbacks" || true)
    if [ -n "${leaked}" ]; then
        echo "ERROR: Forbidden internal symbol leaked in ${so_file}: ${leaked}" >&2
        exit 1
    fi

    echo "✓ ${so_file} ELF verification passed!"
}

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
        "-I${ROOT_DIR}/third_party/json"
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
        -Wl,-soname,arm64-v8a.so \
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
        -I"${ROOT_DIR}/third_party/json" \
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
        -I"${ROOT_DIR}/third_party/json" \
        -Wl,-z,max-page-size=16384 \
        "${ROOT_DIR}/tools/test_fixture/fixture_main.cpp" \
        -o "${TARGET_FIXTURE}"
    "${STRIP}" --strip-unneeded "${TARGET_FIXTURE}"
    chmod 0755 "${TARGET_FIXTURE}"

    verify_shared_library "${TARGET_SO}" "AArch64" "arm64-v8a.so" "${READELF}"
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
        "-I${ROOT_DIR}/third_party/json"
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
        -Wl,-soname,x86_64.so \
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
        -I"${ROOT_DIR}/third_party/json" \
        -Wl,-z,max-page-size=16384 \
        "${ROOT_DIR}/cli/helperctl/helperctl.cpp" \
        -o "${TARGET_CTL}"
    "${STRIP}" --strip-unneeded "${TARGET_CTL}"
    chmod 0755 "${TARGET_CTL}"

    verify_shared_library "${TARGET_SO}" "Advanced Micro Devices X86-64" "x86_64.so" "${READELF}"
    echo "✓ x86_64 build verified successfully!"
}

build_armeabi_v7a() {
    echo "=========================================================="
    echo "  Building KaoriosHelper for armeabi-v7a (Android API ${API_LEVEL})"
    echo "=========================================================="

    local CC="${LLVM_BIN}/armv7a-linux-androideabi${API_LEVEL}-clang"
    local CXX="${LLVM_BIN}/armv7a-linux-androideabi${API_LEVEL}-clang++"
    local STRIP="${LLVM_BIN}/llvm-strip"
    local READELF="${LLVM_BIN}/llvm-readelf"

    local BUILD_DIR="/tmp/kaorios_build_arm32"
    rm -rf "${BUILD_DIR}"
    mkdir -p "${BUILD_DIR}"

    local INCLUDES=(
        "-I${ROOT_DIR}/third_party/zygisk"
        "-I${ROOT_DIR}/third_party/json"
        "-I${ROOT_DIR}/zygisk/include"
        "-I${ROOT_DIR}/engines/hma"
        "-I${ROOT_DIR}/engines/pif"
        "-I${ROOT_DIR}/engines/tee"
        "-I${ROOT_DIR}/engines/copg"
        "-I${ROOT_DIR}/third_party/shadowhook/include"
        "-I${ROOT_DIR}/third_party/shadowhook"
        "-I${ROOT_DIR}/third_party/shadowhook/common"
        "-I${ROOT_DIR}/third_party/shadowhook/arch/arm"
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
        "${ROOT_DIR}/third_party/shadowhook/arch/arm/sh_a32.c"
        "${ROOT_DIR}/third_party/shadowhook/arch/arm/sh_t16.c"
        "${ROOT_DIR}/third_party/shadowhook/arch/arm/sh_t32.c"
        "${ROOT_DIR}/third_party/shadowhook/arch/arm/sh_txx.c"
        "${ROOT_DIR}/third_party/shadowhook/arch/arm/sh_inst.c"
        "${ROOT_DIR}/third_party/shadowhook/arch/arm/sh_glue.S"
        "${ROOT_DIR}/third_party/shadowhook/third_party/xdl/xdl.c"
        "${ROOT_DIR}/third_party/shadowhook/third_party/xdl/xdl_iterate.c"
        "${ROOT_DIR}/third_party/shadowhook/third_party/xdl/xdl_linker.c"
        "${ROOT_DIR}/third_party/shadowhook/third_party/xdl/xdl_lzma.c"
        "${ROOT_DIR}/third_party/shadowhook/third_party/xdl/xdl_util.c"
    )

    echo "Compiling ShadowHook Bionic hook engine (ARM32)..."
    for src in "${SH_SRCS[@]}"; do
        local obj="${BUILD_DIR}/$(basename "${src}").o"
        "${CC}" -O3 -fPIC -fvisibility=hidden -mthumb "${INCLUDES[@]}" -c "${src}" -o "${obj}"
    done

    local CORE_SRCS=(
        "${ROOT_DIR}/zygisk/src/dispatcher.cpp"
        "${ROOT_DIR}/engines/hma/hma_engine.cpp"
        "${ROOT_DIR}/engines/pif/pif_engine.cpp"
        "${ROOT_DIR}/engines/tee/tee_engine.cpp"
        "${ROOT_DIR}/engines/copg/copg_engine.cpp"
        "${ROOT_DIR}/zygisk/src/entry.cpp"
    )

    echo "Compiling Unified Engine Runtime (C++20, ARM32)..."
    for src in "${CORE_SRCS[@]}"; do
        local obj="${BUILD_DIR}/$(basename "${src}").o"
        "${CXX}" -std=c++20 -O3 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -mthumb "${INCLUDES[@]}" -c "${src}" -o "${obj}"
    done

    local TARGET_SO="${ZYGISK_DIR}/armeabi-v7a.so"
    echo "Linking ${TARGET_SO} (Bionic libc)..."
    "${CXX}" -shared -O3 -static-libstdc++ \
        -Wl,-soname,armeabi-v7a.so \
        -Wl,--version-script="${VERSION_SCRIPT}" \
        -Wl,--gc-sections \
        "${BUILD_DIR}"/*.o \
        -llog \
        -o "${TARGET_SO}"

    "${STRIP}" --strip-unneeded "${TARGET_SO}"

    local TARGET_CTL="${OUT_MODULE_DIR}/bin/armeabi-v7a/helperctl"
    mkdir -p "${OUT_MODULE_DIR}/bin/armeabi-v7a"
    echo "Compiling native helperctl CLI for Android ARM32..."
    "${CXX}" -std=c++20 -O3 -static-libstdc++ -pie -fPIE -mthumb \
        -I"${ROOT_DIR}/third_party/json" \
        "${ROOT_DIR}/cli/helperctl/helperctl.cpp" \
        -o "${TARGET_CTL}"
    "${STRIP}" --strip-unneeded "${TARGET_CTL}"
    chmod 0755 "${TARGET_CTL}"

    verify_shared_library "${TARGET_SO}" "ARM" "armeabi-v7a.so" "${READELF}"
    echo "✓ armeabi-v7a build verified successfully!"
}

build_x86() {
    echo "=========================================================="
    echo "  Building KaoriosHelper for x86 (Android API ${API_LEVEL})"
    echo "=========================================================="

    local CC="${LLVM_BIN}/i686-linux-android${API_LEVEL}-clang"
    local CXX="${LLVM_BIN}/i686-linux-android${API_LEVEL}-clang++"
    local STRIP="${LLVM_BIN}/llvm-strip"
    local READELF="${LLVM_BIN}/llvm-readelf"

    local BUILD_DIR="/tmp/kaorios_build_x86"
    rm -rf "${BUILD_DIR}"
    mkdir -p "${BUILD_DIR}"

    local INCLUDES=(
        "-I${ROOT_DIR}/third_party/zygisk"
        "-I${ROOT_DIR}/third_party/json"
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
        "${ROOT_DIR}/third_party/shadowhook/arch/x86/shadowhook_x86_stub.c"
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

    local TARGET_SO="${ZYGISK_DIR}/x86.so"
    echo "Linking ${TARGET_SO} (Bionic libc)..."
    "${CXX}" -shared -O3 -static-libstdc++ \
        -Wl,-soname,x86.so \
        -Wl,--version-script="${VERSION_SCRIPT}" \
        -Wl,--gc-sections \
        "${BUILD_DIR}"/*.o \
        -llog \
        -o "${TARGET_SO}"

    "${STRIP}" --strip-unneeded "${TARGET_SO}"

    local TARGET_CTL="${OUT_MODULE_DIR}/bin/x86/helperctl"
    mkdir -p "${OUT_MODULE_DIR}/bin/x86"
    echo "Compiling native helperctl CLI for Android x86..."
    "${CXX}" -std=c++20 -O3 -static-libstdc++ -pie -fPIE \
        -I"${ROOT_DIR}/third_party/json" \
        "${ROOT_DIR}/cli/helperctl/helperctl.cpp" \
        -o "${TARGET_CTL}"
    "${STRIP}" --strip-unneeded "${TARGET_CTL}"
    chmod 0755 "${TARGET_CTL}"

    verify_shared_library "${TARGET_SO}" "Intel 80386" "x86.so" "${READELF}"
    echo "✓ x86 build verified successfully!"
}

case "${BUILD_TARGET}" in
    arm64|arm64-v8a)
        build_arm64
        ;;
    x86_64)
        build_x86_64
        ;;
    armeabi-v7a|arm)
        build_armeabi_v7a
        ;;
    x86)
        build_x86
        ;;
    all)
        build_arm64
        build_x86_64
        build_armeabi_v7a
        build_x86
        ;;
    *)
        echo "Usage: $0 [arm64-v8a|x86_64|armeabi-v7a|x86|all]"
        exit 1
        ;;
esac

echo "=========================================================="
echo "  Cross-Compilation Complete! Output files in module/"
echo "=========================================================="
ls -lh "${ZYGISK_DIR}"/*.so "${OUT_MODULE_DIR}/helperctl"
