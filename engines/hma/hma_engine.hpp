/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#pragma once

#include <string>
#include <set>
#include <map>
#include <mutex>
#include <atomic>
#include <unistd.h>
#include "engine.hpp"

namespace kaorios {

class HmaEngine : public IEngine {
public:
    explicit HmaEngine(std::string data_dir = "/data/adb/modules/kaorioshelper/data");
    virtual ~HmaEngine() override;

    virtual const char* getName() const override { return "hma"; }
    virtual bool isEnabled() const override;

    virtual bool shouldHandleApp(const std::string &process_name, jint uid) override;
    virtual bool shouldHandleServer() override;

    virtual void onLoad(zygisk::Api *api, JNIEnv *env) override;
    virtual void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override;
    virtual void postServerSpecialize(const zygisk::ServerSpecializeArgs *args) override;

    // Scope check: Returns true if target_pkg should be hidden from caller_pkg / calling_uid
    bool shouldHide(int calling_uid, const std::string &target_pkg) const;

    // Direct DEX loading & entrypoint invocation
    bool tryLoadDex(int module_dir, const char *package_name, const char *process_name);

    // Dynamic config reload
    bool reloadConfigIfNeeded();

    // Query stats
    uint64_t getFilteredCount() const { return filter_count_.load(); }

    // Test helper to inspect scope
    size_t getScopeSize() const {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return scope_map_.size();
    }

private:
    std::string data_dir_;
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    jclass dex_entrypoint_ = nullptr;

    bool loadConfig();
    bool config_loaded_ = false;
    time_t last_config_mtime_ = 0;
    mutable std::mutex config_mutex_;

    // Parsed scope: caller_package -> set of packages to hide
    std::map<std::string, std::set<std::string>> scope_map_;
    // Set of all hidden packages globally (e.g. root managers, Xposed, Magisk)
    std::set<std::string> global_hidden_packages_;
    mutable std::atomic<uint64_t> filter_count_{0};

    void cleanupDex();
};

} // namespace kaorios
