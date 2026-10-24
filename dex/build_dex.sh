#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
# Build script for HMA Zygote classes.dex payload

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC_DIR="${SCRIPT_DIR}/src"
OUT_DIR="${SCRIPT_DIR}/build"
CLASSES_DIR="${OUT_DIR}/classes"
DEX_DIR="${OUT_DIR}/dex"

mkdir -p "${CLASSES_DIR}" "${DEX_DIR}"

# Locate javac
JAVAC_BIN="$(command -v javac || true)"
if [ -z "${JAVAC_BIN}" ]; then
  echo "Error: javac not found. A full JDK is required to build HMA DEX." >&2
  exit 1
fi

# Locate d8
D8_BIN="$(command -v d8 || true)"
if [ -z "${D8_BIN}" ] && [ -n "${ANDROID_HOME:-}" ]; then
  D8_BIN="$(find "${ANDROID_HOME}/build-tools" -name d8 -type f 2>/dev/null | sort -V | tail -n 1 || true)"
fi
if [ -z "${D8_BIN}" ] && [ -n "${ANDROID_SDK_ROOT:-}" ]; then
  D8_BIN="$(find "${ANDROID_SDK_ROOT}/build-tools" -name d8 -type f 2>/dev/null | sort -V | tail -n 1 || true)"
fi

if [ -z "${D8_BIN}" ]; then
  echo "Error: d8 tool not found in PATH, ANDROID_HOME, or ANDROID_SDK_ROOT." >&2
  exit 1
fi

echo "Compiling HMA DEX sources with javac..."
find "${SRC_DIR}" -name "*.java" > "${OUT_DIR}/sources.txt"
"${JAVAC_BIN}" -source 1.8 -target 1.8 -d "${CLASSES_DIR}" @"${OUT_DIR}/sources.txt"

echo "Converting bytecode to DEX with d8..."
CLASS_FILES=$(find "${CLASSES_DIR}" -name "*.class")
"${D8_BIN}" --min-api 26 --output "${DEX_DIR}" ${CLASS_FILES}

if [ -f "${DEX_DIR}/classes.dex" ]; then
  echo "Successfully built ${DEX_DIR}/classes.dex"
  mkdir -p "${ROOT_DIR}/module" "${ROOT_DIR}/module/data"
  cp "${DEX_DIR}/classes.dex" "${ROOT_DIR}/module/classes.dex"
  cp "${DEX_DIR}/classes.dex" "${ROOT_DIR}/module/data/classes.dex"
  echo "Bundled classes.dex into module/ and module/data/"
else
  echo "Error: classes.dex was not generated!" >&2
  exit 1
fi
