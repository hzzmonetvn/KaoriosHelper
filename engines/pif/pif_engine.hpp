/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#pragma once

#include <string>
#include <map>
#include <mutex>
#include "engine.hpp"

namespace kaorios {

class PifEngine : public IEngine {
public:
    explicit PifEngine(std::string data_dir = "/data/adb/modules/kaorioshelper/data");
    virtual ~PifEngine() override = default;

    virtual const char* getName() const override { return "pif"; }
    virtual bool isEnabled() const override;

    virtual bool shouldHandleApp(const std::string &process_name, jint uid) override;
    virtual bool shouldHandleServer() override { return false; }

    virtual void onLoad(zygisk::Api *api, JNIEnv *env) override;
    virtual void preAppSpecialize(zygisk::AppSpecializeArgs *args, const std::string &process_name) override;
    virtual void postAppSpecialize(const zygisk::AppSpecializeArgs *args, const std::string &process_name) override;

    // Helper to evaluate TEE coexistence
    bool isTeeActive() const;

    // Native libc property hooking status
    bool isHookInstalled() const { return hook_installed_; }

    // Test & diagnostic property inspection
    const std::map<std::string, std::string>& getSpoofedProperties() const { return cached_props_; }

    // Simulates property modification callback (used both by native ShadowHook and host tests)
    static bool resolveSpoofedProperty(const std::string &name, const char *orig_value, std::string &out_value);

private:
    std::string data_dir_;
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;

    bool loadConfig(std::map<std::string, std::string> &props, int &spoof_provider);
    bool config_loaded_ = false;
    bool hook_installed_ = false;
    std::map<std::string, std::string> cached_props_;
    std::map<std::string, std::string> last_valid_props_;
    int cached_spoof_provider_ = 0;
    int device_initial_sdk_int_ = 0;
    int spoof_props_ = 1;
    int spoof_build_ = 1;
    uint64_t applied_generation_ = 0;

    void applyBuildField(JNIEnv *env, jclass build_class, const char *name, const std::string &val);
    void applyVersionField(JNIEnv *env, jclass version_class, const char *name, const std::string &val);
    void applyIntField(JNIEnv *env, jclass clazz, const char *name, int val);

    // Native Bionic libc property interception via ByteDance ShadowHook
    void installPropertyHook();
};

} // namespace kaorios
