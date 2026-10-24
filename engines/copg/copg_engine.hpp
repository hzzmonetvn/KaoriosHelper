/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <atomic>
#include <sys/types.h>
#include "engine.hpp"

namespace kaorios {

struct CopgDeviceInfo {
    std::string model;
    std::string brand;
    std::string device;
    std::string manufacturer;
    std::string fingerprint;
    std::string product;
    std::string release;
    std::string serial;
    std::string android_id;
    int sdk_int = 0;
    bool should_spoof_serial = false;
    bool should_spoof_android_id = false;
    bool should_spoof_version = false;
    bool should_spoof_sdk_int = false;
    std::map<std::string, std::string> prop_overrides;
};

struct CopgPackageFlags {
    bool needs_device_spoof = false;
    bool needs_cpu_mount = false;
    bool needs_cpu_unmount = false;
    bool needs_plt_hook = false;
    bool needs_android_id = false;
};

class CopgEngine : public IEngine {
public:
    explicit CopgEngine(std::string data_dir = "/data/adb/modules/kaorioshelper/data");
    virtual ~CopgEngine() override = default;

    virtual const char* getName() const override { return "copg"; }
    virtual bool isEnabled() const override;

    virtual bool shouldHandleApp(const std::string &process_name, jint uid) override;
    virtual bool shouldHandleServer() override { return false; }

    virtual void onLoad(zygisk::Api *api, JNIEnv *env) override;
    virtual void preAppSpecialize(zygisk::AppSpecializeArgs *args, const std::string &process_name) override;
    virtual void postAppSpecialize(const zygisk::AppSpecializeArgs *args, const std::string &process_name) override;

    // Checks if a package name is permitted under the hook ownership policy
    bool isPackageAllowed(const std::string &package_name) const;

    // MemoryIntArray stealth cache poisoning for android_id on android.provider.Settings$Secure.sNameValueCache
    void forgeAndroidId(JNIEnv *env, const char *fake_id);

    // Companion communication for root mount virtualization
    bool executeCompanionCommand(const std::string &command);

    // Dynamic config reload
    bool reloadConfigIfNeeded(bool force = false);

    // PLT / GOT property hook registration
    void installPltPropertyHooks(const std::string &process_name);

    // Dynamic property override lookup for native PLT hooks
    bool resolvePropertyOverride(const std::string &name, std::string &out_value) const;
    void recordPropertyIntercepted() { properties_intercepted_++; }
    uint64_t getInterceptedCount() const { return properties_intercepted_.load(); }
    static CopgEngine* getActiveInstance();

private:
    std::string data_dir_;
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;

    mutable std::mutex config_mutex_;
    time_t last_config_mtime_ = 0;

    std::set<std::string> target_packages_;
    std::set<std::string> cpu_blacklist_;
    std::set<std::string> cpu_only_packages_;
    std::map<std::string, std::pair<CopgDeviceInfo, CopgPackageFlags>> package_configs_;

    CopgDeviceInfo current_device_info_;
    CopgPackageFlags current_flags_;
    bool active_for_current_process_ = false;

    std::atomic<uint64_t> properties_intercepted_{0};
    bool plt_commit_succeeded_ = false;
    bool cpu_mount_active_ = false;
    size_t libraries_discovered_{0};
    size_t hooks_attempted_{0};
    size_t hooks_registered_{0};
    uint64_t applied_generation_{0};

    void applyBuildSpoofing(const CopgDeviceInfo &info);
};

} // namespace kaorios
