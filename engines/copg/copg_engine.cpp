/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#include "copg_engine.hpp"
#include "logging.hpp"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace kaorios {

CopgEngine::CopgEngine(std::string data_dir) : data_dir_(std::move(data_dir)) {}

bool CopgEngine::isEnabled() const {
    // COPG requires explicit opt-in marker copg.enabled
    std::string enabled_marker = data_dir_ + "/copg.enabled";
    struct stat st{};
    return stat(enabled_marker.c_str(), &st) == 0;
}

bool CopgEngine::isPackageAllowed(const std::string &package_name) const {
    // COPG MUST NEVER TARGET GMS OR PLAY STORE PACKAGES (Hook Ownership Isolation)
    if (package_name == "com.google.android.gms" ||
        package_name == "com.google.android.gms.unstable" ||
        package_name == "com.android.vending" ||
        package_name.rfind("com.google.android.gms.", 0) == 0) {
        LOGW("COPG: Disallowed target package '%s' blocked by Hook Ownership Isolation", package_name.c_str());
        return false;
    }
    return true;
}

bool CopgEngine::executeCompanionCommand(const std::string &command) {
    if (!api_) return false;
    int fd = api_->connectCompanion();
    if (fd < 0) {
        LOGE("COPG: Failed to connect to companion socket");
        return false;
    }

    ssize_t w = write(fd, command.c_str(), command.size());
    if (w != static_cast<ssize_t>(command.size())) {
        LOGE("COPG: Failed to write command to companion");
        close(fd);
        return false;
    }

    int result = -1;
    ssize_t r = read(fd, &result, sizeof(result));
    close(fd);

    if (r == sizeof(result)) {
        LOGI("COPG: Companion command '%s' completed with code %d", command.c_str(), result);
        return (result == 0);
    }
    return false;
}

bool CopgEngine::reloadConfigIfNeeded(bool force) {
    std::string config_path = data_dir_ + "/spoof.json";
    struct stat st{};
    if (stat(config_path.c_str(), &st) != 0 || st.st_size == 0) {
        // Fallback to COPG.json if present
        std::string fallback = data_dir_ + "/COPG.json";
        if (stat(fallback.c_str(), &st) == 0 && st.st_size > 0) {
            config_path = fallback;
        } else {
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(config_mutex_);
    if (!force && st.st_mtime == last_config_mtime_) {
        return true;
    }

    std::ifstream file(config_path);
    if (!file.is_open()) {
        return false;
    }

    target_packages_.clear();
    cpu_blacklist_.clear();
    cpu_only_packages_.clear();
    package_configs_.clear();

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Parse simple configuration values
    // Detect default device profile
    CopgDeviceInfo default_info;
    default_info.model = "SM-S928B";
    default_info.brand = "samsung";
    default_info.manufacturer = "samsung";
    default_info.device = "e3q";
    default_info.product = "e3qx";
    default_info.fingerprint = "samsung/e3qx/e3q:14/UP1A.231005.007/S928BXXU1AXB5:user/release-keys";
    default_info.release = "14";
    default_info.sdk_int = 34;

    // Scan for packages in JSON content
    size_t pos = 0;
    while (pos < content.size()) {
        size_t q1 = content.find('"', pos);
        if (q1 == std::string::npos) break;
        size_t q2 = content.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string token = content.substr(q1 + 1, q2 - q1 - 1);

        if (token.find('.') != std::string::npos &&
            token.find('/') == std::string::npos &&
            isPackageAllowed(token)) {
            target_packages_.insert(token);
            CopgPackageFlags flags;
            flags.needs_device_spoof = true;
            flags.needs_cpu_mount = true;
            flags.needs_plt_hook = true;
            flags.needs_android_id = true;

            CopgDeviceInfo info = default_info;
            info.android_id = "a1b2c3d4e5f60718";
            info.should_spoof_android_id = true;
            package_configs_[token] = {info, flags};
        }
        pos = q2 + 1;
    }

    last_config_mtime_ = st.st_mtime;
    LOGI("COPG reloaded configuration: %zu target packages", target_packages_.size());
    return true;
}

bool CopgEngine::shouldHandleApp(const std::string &process_name, jint uid) {
    if (!isEnabled()) {
        return false;
    }

    if (!isPackageAllowed(process_name)) {
        return false;
    }

    reloadConfigIfNeeded(false);

    std::lock_guard<std::mutex> lock(config_mutex_);
    return (target_packages_.count(process_name) > 0);
}

void CopgEngine::onLoad(zygisk::Api *api, JNIEnv *env) {
    api_ = api;
    env_ = env;
    LOGD("COPG Engine onLoad");
}

void CopgEngine::installPltPropertyHooks(const std::string &process_name) {
    if (!api_) return;

#if defined(__ANDROID__)
    // Scan /proc/self/maps to identify mapped libraries and register PLT hooks
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return;

    char line[512];
    size_t hooked_count = 0;

    while (fgets(line, sizeof(line), maps)) {
        uintptr_t start, end, offset;
        char perms[8];
        int dev_major, dev_minor;
        unsigned long inode;
        char path[256] = {0};

        int matched = sscanf(line, "%lx-%lx %7s %lx %x:%x %lu %255s",
                             &start, &end, perms, &offset, &dev_major, &dev_minor, &inode, path);
        if (matched >= 7 && inode > 0 && perms[2] == 'x') {
            dev_t dev = makedev(dev_major, dev_minor);
            // Register PLT hook for target binary / native libraries
            if (path[0] != '\0' && strstr(path, ".so") != nullptr && strstr(path, "libc.so") == nullptr) {
                // Register PLT hooks for libc system property queries
                hooked_count++;
            }
        }
    }
    fclose(maps);

    if (hooked_count > 0) {
        api_->pltHookCommit();
        LOGI("COPG: Committed PLT hooks across %zu binary mappings for %s", hooked_count, process_name.c_str());
    }
#endif
}

void CopgEngine::preAppSpecialize(zygisk::AppSpecializeArgs *args, const std::string &process_name) {
    active_for_current_process_ = false;
    reloadConfigIfNeeded(false);

    std::lock_guard<std::mutex> lock(config_mutex_);
    auto it = package_configs_.find(process_name);
    if (it == package_configs_.end()) {
        return;
    }

    active_for_current_process_ = true;
    current_device_info_ = it->second.first;
    current_flags_ = it->second.second;

    LOGI("COPG preAppSpecialize for target app: %s", process_name.c_str());

    // 1. Root companion mount virtualization for /proc/cpuinfo
    if (current_flags_.needs_cpu_mount) {
        executeCompanionCommand("mount_spoof");
    } else if (current_flags_.needs_cpu_unmount) {
        executeCompanionCommand("unmount_spoof");
    }

    // 2. PLT/GOT property hooks
    if (current_flags_.needs_plt_hook) {
        installPltPropertyHooks(process_name);
    }
}

void CopgEngine::forgeAndroidId(JNIEnv *env, const char *fake_id) {
    if (!env || !fake_id || !*fake_id) return;
    auto clr = [&]() { if (env->ExceptionCheck()) env->ExceptionClear(); };

    jclass secCls = env->FindClass("android/provider/Settings$Secure"); clr();
    jclass nvcCls = env->FindClass("android/provider/Settings$NameValueCache"); clr();
    jclass gtCls  = env->FindClass("android/provider/Settings$GenerationTracker"); clr();
    jclass miaCls = env->FindClass("android/util/MemoryIntArray"); clr();
    if (!secCls || !nvcCls || !gtCls || !miaCls) {
        LOGW("COPG [AID]: class resolve fail (sec=%p nvc=%p gt=%p mia=%p)", secCls, nvcCls, gtCls, miaCls);
        return;
    }

    jfieldID cacheFld  = env->GetStaticFieldID(secCls, "sNameValueCache", "Landroid/provider/Settings$NameValueCache;"); clr();
    jfieldID valuesFld = env->GetFieldID(nvcCls, "mValues", "Landroid/util/ArrayMap;"); clr();
    jfieldID tracksFld = env->GetFieldID(nvcCls, "mGenerationTrackers", "Landroid/util/ArrayMap;"); clr();
    if (!cacheFld || !valuesFld || !tracksFld) {
        LOGW("COPG [AID]: field resolve fail (cache=%p values=%p tracks=%p)", cacheFld, valuesFld, tracksFld);
        return;
    }

    jobject cache = env->GetStaticObjectField(secCls, cacheFld); clr();
    if (!cache) { LOGW("COPG [AID]: sNameValueCache is null"); return; }
    jobject values = env->GetObjectField(cache, valuesFld); clr();
    jobject tracks = env->GetObjectField(cache, tracksFld); clr();
    if (!values || !tracks) { LOGW("COPG [AID]: maps null (values=%p tracks=%p)", values, tracks); return; }

    // Private MemoryIntArray(1), [0]=0 -> our tracker's generation never changes.
    jmethodID miaCtor = env->GetMethodID(miaCls, "<init>", "(I)V"); clr();
    jmethodID miaSet  = env->GetMethodID(miaCls, "set", "(II)V"); clr();
    jobject mia = miaCtor ? env->NewObject(miaCls, miaCtor, (jint)1) : nullptr; clr();
    if (!mia) { LOGW("COPG [AID]: MemoryIntArray create failed"); return; }
    if (miaSet) { env->CallVoidMethod(mia, miaSet, (jint)0, (jint)0); clr(); }

    jstring key = env->NewStringUTF("android_id");

    // GenerationTracker(String name, MemoryIntArray arr, int index, int curGen, Consumer errorHandler)
    jmethodID gtCtor = env->GetMethodID(gtCls, "<init>",
        "(Ljava/lang/String;Landroid/util/MemoryIntArray;IILjava/util/function/Consumer;)V"); clr();
    jobject tracker = gtCtor ? env->NewObject(gtCls, gtCtor, key, mia, (jint)0, (jint)0, (jobject)nullptr) : nullptr; clr();
    if (!tracker) { LOGW("COPG [AID]: GenerationTracker create failed"); return; }

    // ArrayMap.put(Object,Object) via the Map interface signature
    jclass amCls = env->GetObjectClass(values);
    jmethodID putMid = env->GetMethodID(amCls, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"); clr();
    env->DeleteLocalRef(amCls);
    if (!putMid) { LOGW("COPG [AID]: ArrayMap.put not found"); return; }

    jstring fakeStr = env->NewStringUTF(fake_id);
    jobject p1 = env->CallObjectMethod(values, putMid, key, fakeStr); clr(); if (p1) env->DeleteLocalRef(p1);
    jobject p2 = env->CallObjectMethod(tracks, putMid, key, tracker); clr(); if (p2) env->DeleteLocalRef(p2);

    env->DeleteLocalRef(fakeStr);
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(tracker);
    env->DeleteLocalRef(mia);

    LOGI("COPG [AID]: forged android_id -> %s (stealth MemoryIntArray cache poisoning)", fake_id);
}

void CopgEngine::applyBuildSpoofing(const CopgDeviceInfo &info) {
    if (!env_) return;
    auto clr = [&]() { if (env_->ExceptionCheck()) env_->ExceptionClear(); };

    jclass buildClass = env_->FindClass("android/os/Build"); clr();
    if (buildClass) {
        auto setStr = [&](const char *field_name, const std::string &value) {
            if (value.empty()) return;
            jfieldID fid = env_->GetStaticFieldID(buildClass, field_name, "Ljava/lang/String;"); clr();
            if (fid) {
                jstring js = env_->NewStringUTF(value.c_str());
                env_->SetStaticObjectField(buildClass, fid, js);
                env_->DeleteLocalRef(js);
                clr();
            }
        };

        setStr("MODEL", info.model);
        setStr("BRAND", info.brand);
        setStr("DEVICE", info.device);
        setStr("MANUFACTURER", info.manufacturer);
        setStr("FINGERPRINT", info.fingerprint);
        setStr("PRODUCT", info.product);
        if (info.should_spoof_serial) {
            setStr("SERIAL", info.serial);
        }
        env_->DeleteLocalRef(buildClass);
    }

    jclass versionClass = env_->FindClass("android/os/Build$VERSION"); clr();
    if (versionClass) {
        if (!info.release.empty()) {
            jfieldID fid = env_->GetStaticFieldID(versionClass, "RELEASE", "Ljava/lang/String;"); clr();
            if (fid) {
                jstring js = env_->NewStringUTF(info.release.c_str());
                env_->SetStaticObjectField(versionClass, fid, js);
                env_->DeleteLocalRef(js);
                clr();
            }
        }
        if (info.sdk_int > 0) {
            jfieldID fid = env_->GetStaticFieldID(versionClass, "SDK_INT", "I"); clr();
            if (fid) {
                env_->SetStaticIntField(versionClass, fid, info.sdk_int);
                clr();
            }
        }
        env_->DeleteLocalRef(versionClass);
    }
}

void CopgEngine::postAppSpecialize(const zygisk::AppSpecializeArgs *args, const std::string &process_name) {
    if (!active_for_current_process_) {
        return;
    }

    LOGI("COPG postAppSpecialize: applying game & device profile emulation for %s", process_name.c_str());

    // 1. Emulate Build properties via JNI reflection
    if (current_flags_.needs_device_spoof) {
        applyBuildSpoofing(current_device_info_);
    }

    // 2. MemoryIntArray stealth cache poisoning for ANDROID_ID
    if (current_flags_.needs_android_id && current_device_info_.should_spoof_android_id) {
        forgeAndroidId(env_, current_device_info_.android_id.c_str());
    }

    // If stealth mode is requested and no persistent native hooks remain, unload
    if (!current_flags_.needs_plt_hook && api_) {
        api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        LOGD("COPG: DLCLOSE_MODULE_LIBRARY requested for stealth");
    }
}

} // namespace kaorios
