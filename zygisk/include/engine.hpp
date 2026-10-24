/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#pragma once

#include <string>
#include <memory>
#include "zygisk.hpp"

namespace kaorios {

/**
 * Interface representing an isolated subsystem engine within KaoriosHelper.
 */
class IEngine {
public:
    virtual ~IEngine() = default;

    // Unique engine identifier ("hma", "pif", "tee", "copg")
    virtual const char* getName() const = 0;

    // Returns whether this engine is currently enabled in config/storage
    virtual bool isEnabled() const = 0;

    // Invoked on module load in Zygote
    virtual void onLoad(zygisk::Api *api, JNIEnv *env) {}

    // Evaluates whether this engine needs to specialize into the specified app process
    virtual bool shouldHandleApp(const std::string &process_name, jint uid) { return false; }

    // Evaluates whether this engine needs to specialize into system_server
    virtual bool shouldHandleServer() { return false; }

    // Lifecycle hooks for app specialization
    virtual void preAppSpecialize(zygisk::AppSpecializeArgs *args, const std::string &process_name) {}
    virtual void postAppSpecialize(const zygisk::AppSpecializeArgs *args, const std::string &process_name) {}

    // Lifecycle hooks for system_server specialization
    virtual void preServerSpecialize(zygisk::ServerSpecializeArgs *args) {}
    virtual void postServerSpecialize(const zygisk::ServerSpecializeArgs *args) {}
};

} // namespace kaorios
