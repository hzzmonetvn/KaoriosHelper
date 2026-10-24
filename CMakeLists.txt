# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project

cmake_minimum_required(VERSION 3.22)
project(kaorioshelper VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fvisibility-inlines-hidden")

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zygisk
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jni
    ${CMAKE_CURRENT_SOURCE_DIR}/zygisk/include
    ${CMAKE_CURRENT_SOURCE_DIR}/engines/hma
    ${CMAKE_CURRENT_SOURCE_DIR}/engines/pif
    ${CMAKE_CURRENT_SOURCE_DIR}/engines/tee
    ${CMAKE_CURRENT_SOURCE_DIR}/engines/copg
)

# Core sources for dispatcher and engines
set(CORE_SOURCES
    zygisk/src/dispatcher.cpp
    engines/hma/hma_engine.cpp
    engines/pif/pif_engine.cpp
    engines/tee/tee_engine.cpp
    engines/copg/copg_engine.cpp
)

# Unified shared library for Zygisk
add_library(kaorioshelper SHARED
    ${CORE_SOURCES}
    zygisk/src/entry.cpp
)

# Native unit test runner
add_executable(test_native
    ${CORE_SOURCES}
    tests/native/test_dispatcher.cpp
)

# Native helperctl control CLI
add_executable(helperctl
    cli/helperctl/helperctl.cpp
)
