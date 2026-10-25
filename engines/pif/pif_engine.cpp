/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#include "pif_engine.hpp"
#include "logging.hpp"
#if __has_include(<ack_protocol.hpp>)
#include <ack_protocol.hpp>
#elif __has_include("../../include/ack_protocol.hpp")
#include "../../include/ack_protocol.hpp"
#endif
#include <fstream>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
#include <atomic>

static std::atomic<uint64_t> s_properties_intercepted{0};

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#elif __has_include("../../third_party/json/nlohmann/json.hpp")
#include "../../third_party/json/nlohmann/json.hpp"
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#include <dlfcn.h>
#include "shadowhook.h"

#ifndef PROP_VALUE_MAX
#define PROP_VALUE_MAX 92
#endif

typedef void (*T_Callback)(void *cookie, const char *name, const char *value, uint32_t serial);

struct PropertyCallbackContext {
    T_Callback original_callback = nullptr;
    void *original_cookie = nullptr;
};

static void (*o_system_property_read_callback)(const prop_info *, T_Callback, void *) = nullptr;
static int (*o_system_property_get)(const char *name, char *value) = nullptr;

#endif

namespace kaorios {

static std::map<std::string, std::string> s_active_spoof_props;
static std::mutex s_active_props_mutex;

PifEngine::PifEngine(std::string data_dir) : data_dir_(std::move(data_dir)) {}

bool PifEngine::isEnabled() const {
    std::string disabled_marker = data_dir_ + "/pif.disabled";
    struct stat st{};
    return stat(disabled_marker.c_str(), &st) != 0;
}

bool PifEngine::isTeeActive() const {
    std::string tee_enabled_marker = data_dir_ + "/tee.enabled";
    std::string keybox_file = data_dir_ + "/keybox.xml";
    struct stat st_en{}, st_kb{};
    return (stat(tee_enabled_marker.c_str(), &st_en) == 0 &&
            stat(keybox_file.c_str(), &st_kb) == 0 &&
            st_kb.st_size > 0);
}

bool PifEngine::shouldHandleApp(const std::string &process_name, jint uid) {
    if (!isEnabled()) {
        return false;
    }

    // PIF specifically targets Google Play Services and Play Store
    return (process_name == "com.google.android.gms" ||
            process_name == "com.google.android.gms.unstable" ||
            process_name == "com.android.vending");
}

void PifEngine::onLoad(zygisk::Api *api, JNIEnv *env) {
    api_ = api;
    env_ = env;
    LOGD("PIF Engine onLoad");
}

bool PifEngine::resolveSpoofedProperty(const std::string &name, const char *orig_value, std::string &out_value) {
    std::lock_guard<std::mutex> lock(s_active_props_mutex);
    if (s_active_spoof_props.empty()) {
        return false;
    }

    // 1. Exact match
    auto it = s_active_spoof_props.find(name);
    if (it != s_active_spoof_props.end()) {
        out_value = it->second;
        return true;
    }

    // 2. Wildcard prefix/suffix match (e.g. *security_patch, *first_api_level)
    for (const auto &[pattern, val] : s_active_spoof_props) {
        if (pattern.length() > 1 && pattern.front() == '*') {
            std::string_view suffix(pattern.data() + 1, pattern.length() - 1);
            if (name.length() >= suffix.length() &&
                name.compare(name.length() - suffix.length(), suffix.length(), suffix) == 0) {
                out_value = val;
                return true;
            }
        } else if (pattern.length() > 1 && pattern.back() == '*') {
            std::string_view prefix(pattern.data(), pattern.length() - 1);
            if (name.length() >= prefix.length() &&
                name.compare(0, prefix.length(), prefix) == 0) {
                out_value = val;
                return true;
            }
        }
    }

    return false;
}

#if defined(__ANDROID__)
static void pif_modify_callback(void *cookie, const char *name, const char *value, uint32_t serial) {
    auto *ctx = static_cast<PropertyCallbackContext *>(cookie);
    if (!ctx || !ctx->original_callback || !name || !value) return;

    std::string spoofed_val;
    const char *final_val = value;
    if (PifEngine::resolveSpoofedProperty(name, value, spoofed_val)) {
        s_properties_intercepted++;
        final_val = spoofed_val.c_str();
        LOGD("PIF [ShadowHook libc] [%s]: %s -> %s", name, value, final_val);
    }

    ctx->original_callback(ctx->original_cookie, name, final_val, serial);
}

static void my_system_property_read_callback(const prop_info *pi, T_Callback callback, void *cookie) {
    if (!o_system_property_read_callback || !pi || !callback) return;

    // Bionic libc's __system_property_read_callback executes callback synchronously
    // within the caller's stack frame. Allocating PropertyCallbackContext on the stack
    // frame completely eliminates unbounded heap growth, mutex contention, and leaks.
    PropertyCallbackContext ctx{callback, cookie};
    o_system_property_read_callback(pi, pif_modify_callback, &ctx);
}

static int my_system_property_get(const char *name, char *value) {
    if (!name || !value) return 0;
    std::string spoofed_val;
    if (PifEngine::resolveSpoofedProperty(name, "", spoofed_val)) {
        s_properties_intercepted++;
        size_t len = std::min(spoofed_val.size(), static_cast<size_t>(PROP_VALUE_MAX - 1));
        std::memcpy(value, spoofed_val.data(), len);
        value[len] = '\0';
        LOGD("PIF [ShadowHook libc get] [%s] -> %s", name, value);
        return static_cast<int>(len);
    }
    if (o_system_property_get) {
        return o_system_property_get(name, value);
    }
    return 0;
}
#endif

void PifEngine::installPropertyHook() {
    if (hook_installed_) return;

#if defined(__ANDROID__)
#if !defined(__arm__) && !defined(__aarch64__)
    LOGW("PIF: Non-ARM architecture detected. KaoriosHelper is built exclusively for ARM devices (arm64-v8a, armeabi-v7a).");
    hook_installed_ = false;
    return;
#else
    int sh_ret = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    if (sh_ret != 0 && sh_ret != SHADOWHOOK_ERRNO_PENDING) {
        LOGW("PIF: shadowhook_init returned %d (errno %d)", sh_ret, shadowhook_get_errno());
    }

    void *sym_addr = dlsym(RTLD_DEFAULT, "__system_property_read_callback");
    if (sym_addr) {
        void *handle = shadowhook_hook_sym_addr(
            sym_addr,
            reinterpret_cast<void *>(my_system_property_read_callback),
            reinterpret_cast<void **>(&o_system_property_read_callback)
        );
        if (handle) {
            hook_installed_ = true;
            LOGI("PIF: ShadowHook successfully intercepted __system_property_read_callback at %p (handle=%p)", sym_addr, handle);
        } else {
            LOGE("PIF: Failed to hook __system_property_read_callback. ShadowHook errno: %d", shadowhook_get_errno());
        }
    } else {
        LOGE("PIF: Could not resolve __system_property_read_callback via dlsym");
    }

    void *sym_get = dlsym(RTLD_DEFAULT, "__system_property_get");
    if (sym_get) {
        void *handle_get = shadowhook_hook_sym_addr(
            sym_get,
            reinterpret_cast<void *>(my_system_property_get),
            reinterpret_cast<void **>(&o_system_property_get)
        );
        if (handle_get) {
            hook_installed_ = true;
            LOGI("PIF: ShadowHook successfully intercepted __system_property_get at %p (handle=%p)", sym_get, handle_get);
        } else {
            LOGW("PIF: Failed to hook __system_property_get. ShadowHook errno: %d", shadowhook_get_errno());
        }
    }
#endif
#else
    hook_installed_ = true;
    LOGI("PIF: [Mock] Native Bionic libc property hook registered");
#endif
}

bool PifEngine::loadConfig(std::map<std::string, std::string> &props, int &spoof_provider) {
    std::string config_path = data_dir_ + "/pif.json";
    struct stat st{};

    spoof_provider = 0;
    spoof_props_ = 1;
    spoof_build_ = 1;

    if (stat(config_path.c_str(), &st) != 0 || st.st_size == 0) {
        LOGW("PIF: Configuration file missing or empty at %s. Engine inactive until configured.", config_path.c_str());
        props.clear();
        return false;
    }

    std::ifstream file(config_path);
    if (!file.is_open()) {
        LOGW("PIF: Failed to open %s", config_path.c_str());
        if (!last_valid_props_.empty()) {
            LOGI("PIF: Retaining last-known-good user configuration");
            props = last_valid_props_;
            return true;
        }
        props.clear();
        return false;
    }

    nlohmann::json j;
    try {
        file >> j;
        if (!j.is_object()) {
            LOGW("PIF: JSON root is not an object in %s", config_path.c_str());
            if (!last_valid_props_.empty()) {
                LOGI("PIF: Retaining last-known-good user configuration");
                props = last_valid_props_;
                return true;
            }
            props.clear();
            return false;
        }
    } catch (const std::exception &e) {
        LOGW("PIF: Failed to parse %s as JSON (%s)", config_path.c_str(), e.what());
        if (!last_valid_props_.empty()) {
            LOGI("PIF: Retaining last-known-good user configuration");
            props = last_valid_props_;
            return true;
        }
        props.clear();
        return false;
    }

    if (j.empty() || (j.contains("enabled") && !j["enabled"].get<bool>())) {
        LOGI("PIF: Configuration is empty or explicitly disabled");
        props.clear();
        return false;
    }

    props.clear();
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string &key = it.key();
        const auto &val = it.value();

        if (key == "spoofProvider") {
            if (val.is_number()) spoof_provider = val.get<int>();
            else if (val.is_string()) spoof_provider = std::atoi(val.get<std::string>().c_str());
            else if (val.is_boolean()) spoof_provider = val.get<bool>() ? 1 : 0;
        } else if (key == "spoofProps") {
            if (val.is_number()) spoof_props_ = val.get<int>();
            else if (val.is_string()) spoof_props_ = std::atoi(val.get<std::string>().c_str());
            else if (val.is_boolean()) spoof_props_ = val.get<bool>() ? 1 : 0;
        } else if (key == "spoofBuild") {
            if (val.is_number()) spoof_build_ = val.get<int>();
            else if (val.is_string()) spoof_build_ = std::atoi(val.get<std::string>().c_str());
            else if (val.is_boolean()) spoof_build_ = val.get<bool>() ? 1 : 0;
        } else if (key == "DEVICE_INITIAL_SDK_INT" || key == "FIRST_API_LEVEL") {
            if (val.is_number()) device_initial_sdk_int_ = val.get<int>();
            else if (val.is_string()) device_initial_sdk_int_ = std::atoi(val.get<std::string>().c_str());
        } else if (val.is_string()) {
            props[key] = val.get<std::string>();
        } else if (val.is_number_integer()) {
            props[key] = std::to_string(val.get<int64_t>());
        } else if (val.is_boolean()) {
            props[key] = val.get<bool>() ? "1" : "0";
        }
    }

    // Also populate native system properties corresponding to standard Android Build fields
    // DroidGuard (libdroidguard.so) reads these directly from Bionic libc!
    if (props.count("MODEL")) props["ro.product.model"] = props["MODEL"];
    if (props.count("MANUFACTURER")) props["ro.product.manufacturer"] = props["MANUFACTURER"];
    if (props.count("BRAND")) props["ro.product.brand"] = props["BRAND"];
    if (props.count("DEVICE")) props["ro.product.device"] = props["DEVICE"];
    if (props.count("PRODUCT")) props["ro.product.name"] = props["PRODUCT"];
    if (props.count("FINGERPRINT")) props["ro.build.fingerprint"] = props["FINGERPRINT"];
    if (props.count("ID")) props["ro.build.id"] = props["ID"];
    if (props.count("TYPE")) props["ro.build.type"] = props["TYPE"];
    if (props.count("TAGS")) props["ro.build.tags"] = props["TAGS"];
    if (props.count("SECURITY_PATCH")) {
        props["ro.build.version.security_patch"] = props["SECURITY_PATCH"];
        props["*.security_patch"] = props["SECURITY_PATCH"];
    }
    if (props.count("RELEASE")) props["ro.build.version.release"] = props["RELEASE"];
    if (props.count("INCREMENTAL")) props["ro.build.version.incremental"] = props["INCREMENTAL"];
    if (device_initial_sdk_int_ > 0) {
        std::string sdk_str = std::to_string(device_initial_sdk_int_);
        props["ro.product.first_api_level"] = sdk_str;
        props["*first_api_level"] = sdk_str;
    }
    // Hardened boot state properties required to satisfy CTS / Basic & Device Play Integrity
    props["ro.boot.flash.locked"] = "1";
    props["ro.boot.verifiedbootstate"] = "green";
    props["ro.boot.vbmeta.device_state"] = "locked";

    // Enforce hook ownership rule: if TEE is active, force spoofProvider = 0
    if (isTeeActive()) {
        if (spoof_provider != 0) {
            LOGW("PIF: TEE Engine is active! Forcing spoofProvider = 0 to prevent KeyStore collision.");
            spoof_provider = 0;
        }
    }

    // Publish active properties for native libc hook
    {
        std::lock_guard<std::mutex> lock(s_active_props_mutex);
        s_active_spoof_props = props;
    }

    return true;
}

void PifEngine::preAppSpecialize(zygisk::AppSpecializeArgs *args, const std::string &process_name) {
    LOGI("PIF targeting process: %s", process_name.c_str());
    config_loaded_ = loadConfig(cached_props_, cached_spoof_provider_);

    if (api_) {
        // Enforce root unmount in GMS process so DroidGuard cannot scan root mount namespaces
        api_->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);
    }
}

void PifEngine::applyBuildField(JNIEnv *env, jclass build_class, const char *name, const std::string &val) {
    if (!env || !build_class || val.empty()) return;
    jfieldID fid = env->GetStaticFieldID(build_class, name, "Ljava/lang/String;");
    if (fid) {
        jstring jval = env->NewStringUTF(val.c_str());
        env->SetStaticObjectField(build_class, fid, jval);
        env->DeleteLocalRef(jval);
    } else {
        env->ExceptionClear();
    }
}

void PifEngine::applyVersionField(JNIEnv *env, jclass version_class, const char *name, const std::string &val) {
    if (!env || !version_class || val.empty()) return;
    jfieldID fid = env->GetStaticFieldID(version_class, name, "Ljava/lang/String;");
    if (fid) {
        jstring jval = env->NewStringUTF(val.c_str());
        env->SetStaticObjectField(version_class, fid, jval);
        env->DeleteLocalRef(jval);
    } else {
        env->ExceptionClear();
    }
}

void PifEngine::applyIntField(JNIEnv *env, jclass clazz, const char *name, int val) {
    if (!env || !clazz || val <= 0) return;
    jfieldID fid = env->GetStaticFieldID(clazz, name, "I");
    if (fid) {
        env->SetStaticIntField(clazz, fid, val);
    } else {
        env->ExceptionClear();
    }
}

void PifEngine::postAppSpecialize(const zygisk::AppSpecializeArgs *args, const std::string &process_name) {
    if (!config_loaded_) {
        config_loaded_ = loadConfig(cached_props_, cached_spoof_provider_);
    }

    if (!config_loaded_ || cached_props_.empty()) {
        LOGW("PIF: No valid configuration found to apply");
        return;
    }

    // 1. Install ByteDance ShadowHook for native libc property reads (DroidGuard interception)
    if (spoof_props_ > 0) {
        installPropertyHook();
    }

    // 2. Apply Java Build reflection for framework callers
    if (spoof_build_ > 0 && env_) {
        const auto &props = cached_props_;

        jclass build_class = env_->FindClass("android/os/Build");
        if (build_class) {
            if (props.count("MANUFACTURER")) applyBuildField(env_, build_class, "MANUFACTURER", props.at("MANUFACTURER"));
            if (props.count("MODEL")) applyBuildField(env_, build_class, "MODEL", props.at("MODEL"));
            if (props.count("BRAND")) applyBuildField(env_, build_class, "BRAND", props.at("BRAND"));
            if (props.count("DEVICE")) applyBuildField(env_, build_class, "DEVICE", props.at("DEVICE"));
            if (props.count("PRODUCT")) applyBuildField(env_, build_class, "PRODUCT", props.at("PRODUCT"));
            if (props.count("FINGERPRINT")) applyBuildField(env_, build_class, "FINGERPRINT", props.at("FINGERPRINT"));
            if (props.count("ID")) applyBuildField(env_, build_class, "ID", props.at("ID"));
            if (props.count("TYPE")) applyBuildField(env_, build_class, "TYPE", props.at("TYPE"));
            if (props.count("TAGS")) applyBuildField(env_, build_class, "TAGS", props.at("TAGS"));
            env_->DeleteLocalRef(build_class);
        } else {
            env_->ExceptionClear();
        }

        jclass version_class = env_->FindClass("android/os/Build$VERSION");
        if (version_class) {
            if (props.count("RELEASE")) applyVersionField(env_, version_class, "RELEASE", props.at("RELEASE"));
            if (props.count("INCREMENTAL")) applyVersionField(env_, version_class, "INCREMENTAL", props.at("INCREMENTAL"));
            if (props.count("SECURITY_PATCH")) applyVersionField(env_, version_class, "SECURITY_PATCH", props.at("SECURITY_PATCH"));
            if (device_initial_sdk_int_ > 0) {
                applyIntField(env_, version_class, "DEVICE_INITIAL_SDK_INT", device_initial_sdk_int_);
            }
            env_->DeleteLocalRef(version_class);
        } else {
            env_->ExceptionClear();
        }
    }

    LOGI("PIF properties and native hooks specialization completed in %s", process_name.c_str());

    RuntimeAck ack;
    ack.engine = "pif";
    ack.processName = process_name;
    ack.pid = getpid();
    ack.uid = getuid();
    ack.configGeneration = applied_generation_;

    if (hook_installed_) {
        ack.initResult = 0;
        ack.eventType = (s_properties_intercepted.load() > 0) ? AckEventType::HOOK_OBSERVED : AckEventType::HOOK_INITIALIZED;
        ack.detail = "ShadowHook Bionic libc __system_property_read_callback active";
    } else {
        ack.initResult = -1;
        ack.errorCode = ENOSYS;
        ack.eventType = AckEventType::APPLY_FAILED;
#if !defined(__arm__) && !defined(__aarch64__)
        ack.detail = "Architecture unsupported: KaoriosHelper is built exclusively for ARM devices (arm64-v8a, armeabi-v7a)";
#else
        ack.detail = "ShadowHook hook failed or unsupported on this ABI";
#endif
    }

    if (api_) {
        int sock = api_->connectCompanion();
        if (sock >= 0) {
            std::string msg = "ack:" + ack.toJson().dump();
            write(sock, msg.data(), msg.size());
            int resp = -1;
            read(sock, &resp, sizeof(resp));
            close(sock);
        } else if (geteuid() == 0) {
            writeRuntimeAckDirect(data_dir_ + "/runtime", ack);
        }
    } else if (geteuid() == 0) {
        writeRuntimeAckDirect(data_dir_ + "/runtime", ack);
    }
}

} // namespace kaorios
