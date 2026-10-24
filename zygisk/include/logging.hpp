/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#pragma once

#include <cstdio>

#define LOG_TAG "KaoriosHelper"

#if defined(__ANDROID__)
#include <android/log.h>
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGD(...) do { printf("[DEBUG] [" LOG_TAG "] "); printf(__VA_ARGS__); printf("\n"); } while(0)
#define LOGI(...) do { printf("[INFO] [" LOG_TAG "] "); printf(__VA_ARGS__); printf("\n"); } while(0)
#define LOGW(...) do { printf("[WARN] [" LOG_TAG "] "); printf(__VA_ARGS__); printf("\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "[ERROR] [" LOG_TAG "] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif
