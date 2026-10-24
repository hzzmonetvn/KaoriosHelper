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
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#if __has_include(<ack_protocol.hpp>)
#include <ack_protocol.hpp>
#elif __has_include("../../include/ack_protocol.hpp")
#include "../../include/ack_protocol.hpp"
#endif

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#elif __has_include("../../third_party/json/nlohmann/json.hpp")
#include "../../third_party/json/nlohmann/json.hpp"
#endif

#if defined(__ANDROID__)
#include <sys/system_properties.h>

static int (*o_copg_system_property_get)(const char *name, char *value) = nullptr;

static int my_copg_system_property_get(const char *name, char *value) {
    if (!name || !value) {
        if (o_copg_system_property_get) return o_copg_system_property_get(name, value);
        return -1;
    }

    kaorios::CopgEngine *engine = kaorios::CopgEngine::getActiveInstance();
    if (engine) {
        std::string override_val;
        if (engine->resolvePropertyOverride(name, override_val)) {
            engine->recordPropertyIntercepted();
            size_t len = std::min(override_val.size(), static_cast<size_t>(PROP_VALUE_MAX - 1));
            memcpy(value, override_val.c_str(), len);
            value[len] = '\0';
            LOGD("COPG [PLT Hook] [%s]: -> %s", name, value);
            return static_cast<int>(len);
        }
    }

    if (o_copg_system_property_get) {
        return o_copg_system_property_get(name, value);
    }
    return -1;
}
#endif

namespace kaorios {

static CopgEngine *s_copg_active_instance = nullptr;

CopgEngine* CopgEngine::getActiveInstance() {
    return s_copg_active_instance;
}

bool CopgEngine::resolvePropertyOverride(const std::string &name, std::string &out_value) const {
    if (!active_for_current_process_) return false;

    // 1. Explicit property overrides map
    auto it = current_device_info_.prop_overrides.find(name);
    if (it != current_device_info_.prop_overrides.end()) {
        out_value = it->second;
        return true;
    }

    // 2. Standard Android system property mappings
    if (name == "ro.product.model" && !current_device_info_.model.empty()) {
        out_value = current_device_info_.model;
        return true;
    }
    if (name == "ro.product.brand" && !current_device_info_.brand.empty()) {
        out_value = current_device_info_.brand;
        return true;
    }
    if (name == "ro.product.manufacturer" && !current_device_info_.manufacturer.empty()) {
        out_value = current_device_info_.manufacturer;
        return true;
    }
    if (name == "ro.product.device" && !current_device_info_.device.empty()) {
        out_value = current_device_info_.device;
        return true;
    }
    if (name == "ro.product.name" && !current_device_info_.product.empty()) {
        out_value = current_device_info_.product;
        return true;
    }
    if (name == "ro.build.fingerprint" && !current_device_info_.fingerprint.empty()) {
        out_value = current_device_info_.fingerprint;
        return true;
    }

    return false;
}

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

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    auto j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        LOGW("COPG: Malformed JSON in %s; retaining existing known-good config", config_path.c_str());
        return false;
    }

    // Check global enablement
    if (j.contains("enabled") && j["enabled"].is_boolean() && !j["enabled"].get<bool>()) {
        LOGI("COPG: Feature globally disabled in %s", config_path.c_str());
        target_packages_.clear();
        package_configs_.clear();
        last_config_mtime_ = st.st_mtime;
        return true;
    }

    std::set<std::string> new_target_packages;
    std::map<std::string, std::pair<CopgDeviceInfo, CopgPackageFlags>> new_package_configs;
    std::set<std::string> new_cpu_blacklist;
    std::set<std::string> new_cpu_only_packages;

    // 1. Schema-aware parsing of profiles / packages object
    const nlohmann::json *profiles_obj = nullptr;
    if (j.contains("profiles") && j["profiles"].is_object()) {
        profiles_obj = &j["profiles"];
    } else if (j.contains("packages") && j["packages"].is_object()) {
        profiles_obj = &j["packages"];
    }

    if (profiles_obj) {
        for (auto &[pkg_name, prof] : profiles_obj->items()) {
            if (pkg_name.find('.') == std::string::npos || pkg_name.find('/') != std::string::npos) {
                LOGW("COPG: Skipping invalid package identifier: %s", pkg_name.c_str());
                continue;
            }
            if (!isPackageAllowed(pkg_name)) {
                continue;
            }
            if (!prof.is_object()) continue;

            if (prof.contains("enabled") && prof["enabled"].is_boolean() && !prof["enabled"].get<bool>()) {
                continue;
            }

            CopgDeviceInfo info;
            CopgPackageFlags flags;

            flags.needs_device_spoof = prof.value("device_spoof", true);
            flags.needs_cpu_mount = prof.value("cpu_spoof", false); // Default false per Priority 4
            flags.needs_plt_hook = prof.value("plt_hook", false);

            if (prof.contains("model") && prof["model"].is_string()) info.model = prof["model"].get<std::string>();
            if (prof.contains("brand") && prof["brand"].is_string()) info.brand = prof["brand"].get<std::string>();
            if (prof.contains("manufacturer") && prof["manufacturer"].is_string()) info.manufacturer = prof["manufacturer"].get<std::string>();
            if (prof.contains("device") && prof["device"].is_string()) info.device = prof["device"].get<std::string>();
            if (prof.contains("product") && prof["product"].is_string()) info.product = prof["product"].get<std::string>();
            if (prof.contains("fingerprint") && prof["fingerprint"].is_string()) info.fingerprint = prof["fingerprint"].get<std::string>();
            if (prof.contains("release") && prof["release"].is_string()) info.release = prof["release"].get<std::string>();
            if (prof.contains("serial") && prof["serial"].is_string()) {
                info.serial = prof["serial"].get<std::string>();
                info.should_spoof_serial = !info.serial.empty();
            }
            if (prof.contains("sdk_int") && prof["sdk_int"].is_number_integer()) {
                info.sdk_int = prof["sdk_int"].get<int>();
                info.should_spoof_sdk_int = (info.sdk_int > 0);
            }

            // Android ID spoofing: ONLY if explicitly specified in JSON!
            if (prof.contains("android_id") && prof["android_id"].is_string()) {
                info.android_id = prof["android_id"].get<std::string>();
                bool aid_flag = prof.value("android_id_spoof", true);
                info.should_spoof_android_id = (aid_flag && !info.android_id.empty());
                flags.needs_android_id = info.should_spoof_android_id;
            }

            // Custom property overrides
            if (prof.contains("props") && prof["props"].is_object()) {
                for (auto &[pk, pv] : prof["props"].items()) {
                    if (pv.is_string()) {
                        info.prop_overrides[pk] = pv.get<std::string>();
                    }
                }
            }

            new_target_packages.insert(pkg_name);
            new_package_configs[pkg_name] = {info, flags};
        }
    }

    // 2. Also parse top-level string arrays (e.g. PACKAGES_GENSHIN, TARGETS, packages)
    for (auto &[key, val] : j.items()) {
        if (val.is_array()) {
            for (const auto &item : val) {
                if (item.is_string()) {
                    std::string pkg_name = item.get<std::string>();
                    if (pkg_name.find('.') != std::string::npos &&
                        pkg_name.find('/') == std::string::npos &&
                        isPackageAllowed(pkg_name)) {
                        new_target_packages.insert(pkg_name);
                        if (new_package_configs.find(pkg_name) == new_package_configs.end()) {
                            CopgDeviceInfo info;
                            CopgPackageFlags flags;
                            flags.needs_device_spoof = true;
                            flags.needs_cpu_mount = false; // Priority 4: disabled by default
                            flags.needs_plt_hook = true;
                            flags.needs_android_id = false;
                            new_package_configs[pkg_name] = {info, flags};
                        }
                    }
                }
            }
        }
    }

    // 3. Parse cpu_spoof block
    if (j.contains("cpu_spoof") && j["cpu_spoof"].is_object()) {
        const auto &cs = j["cpu_spoof"];
        if (cs.contains("blacklist") && cs["blacklist"].is_array()) {
            for (const auto &item : cs["blacklist"]) {
                if (item.is_string()) new_cpu_blacklist.insert(item.get<std::string>());
            }
        }
        if (cs.contains("cpu_only_packages") && cs["cpu_only_packages"].is_array()) {
            for (const auto &item : cs["cpu_only_packages"]) {
                if (item.is_string()) new_cpu_only_packages.insert(item.get<std::string>());
            }
        }
    }

    target_packages_ = std::move(new_target_packages);
    package_configs_ = std::move(new_package_configs);
    cpu_blacklist_ = std::move(new_cpu_blacklist);
    cpu_only_packages_ = std::move(new_cpu_only_packages);
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
    size_t libraries_discovered = 0;
    size_t hooks_attempted = 0;
    size_t hooks_succeeded = 0;
    bool commit_succeeded = false;

    // Scan /proc/self/maps to identify mapped libraries and register PLT hooks
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        LOGW("COPG: Cannot open /proc/self/maps for PLT hook scan in %s: %s", process_name.c_str(), strerror(errno));
        return;
    }

    char line[512];
    std::set<std::pair<dev_t, unsigned long>> hooked_inodes;

    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0, end = 0, offset = 0;
        char perms[8];
        int dev_major, dev_minor;
        unsigned long inode;
        char path[256] = {0};

        int matched = sscanf(line, "%lx-%lx %7s %lx %x:%x %lu %255s",
                             &start, &end, perms, &offset, &dev_major, &dev_minor, &inode, path);
        if (matched >= 7 && inode > 0 && perms[2] == 'x' && path[0] != '\0') {
            if (strstr(path, ".so") != nullptr) {
                libraries_discovered++;

                // Skip system runtime libraries to prevent recursion and adhere to hook ownership
                if (strstr(path, "libc.so") != nullptr ||
                    strstr(path, "libm.so") != nullptr ||
                    strstr(path, "libdl.so") != nullptr ||
                    strstr(path, "liblog.so") != nullptr ||
                    strstr(path, "libandroid_runtime.so") != nullptr) {
                    continue;
                }

                dev_t dev = makedev(dev_major, dev_minor);
                if (hooked_inodes.count({dev, inode}) == 0) {
                    hooked_inodes.insert({dev, inode});
                    hooks_attempted++;
                    // Register PLT hook for __system_property_get via official Zygisk API v4
                    api_->pltHookRegister(dev, inode, "__system_property_get",
                                          reinterpret_cast<void *>(my_copg_system_property_get),
                                          reinterpret_cast<void **>(&o_copg_system_property_get));
                    hooks_succeeded++;
                }
            }
        }
    }
    fclose(maps);

    if (hooks_attempted > 0) {
        commit_succeeded = api_->pltHookCommit();
        plt_commit_succeeded_ = commit_succeeded;
        LOGI("COPG PLT Hook Status: discovered=%zu, attempted=%zu, registered=%zu, committed=%d for %s",
             libraries_discovered, hooks_attempted, hooks_succeeded, commit_succeeded ? 1 : 0, process_name.c_str());
    } else {
        plt_commit_succeeded_ = false;
        LOGI("COPG: Discovered %zu mapped libraries in %s, but no target native libraries eligible for PLT hooking",
             libraries_discovered, process_name.c_str());
    }
    libraries_discovered_ = libraries_discovered;
    hooks_attempted_ = hooks_attempted;
    hooks_registered_ = hooks_succeeded;
#else
    (void)process_name;
    libraries_discovered_ = 0;
    hooks_attempted_ = 0;
    hooks_registered_ = 0;
    plt_commit_succeeded_ = false;
    LOGD("COPG: [Host Mock] PLT hook installation skipped (not Android)");
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
    s_copg_active_instance = this;
    current_device_info_ = it->second.first;
    current_flags_ = it->second.second;

    LOGI("COPG preAppSpecialize for target app: %s", process_name.c_str());

    // PLT/GOT property hooks are registered in preAppSpecialize before code execution
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

    // 3. Mount namespace isolation check & execution
    cpu_mount_active_ = false;
    if (current_flags_.needs_cpu_mount) {
        bool ns_isolated = false;
        struct stat self_st{}, root_st{};
        if (stat("/proc/self/ns/mnt", &self_st) == 0 && stat("/proc/1/ns/mnt", &root_st) == 0) {
            if (self_st.st_ino != root_st.st_ino) {
                ns_isolated = true;
            }
        }

        if (ns_isolated) {
            std::string spoof_source = data_dir_ + "/cpuinfo_spoof";
            if (access(spoof_source.c_str(), F_OK) != 0) {
                spoof_source = "/data/adb/modules/COPG/cpuinfo_spoof";
            }

            if (access(spoof_source.c_str(), F_OK) == 0) {
                if (api_) {
                    int sock = api_->connectCompanion();
                    if (sock >= 0) {
                        std::string cmd = "mount_cpu:" + std::to_string(getpid()) + ":" + spoof_source;
                        write(sock, cmd.data(), cmd.size());
                        int resp = -1;
                        read(sock, &resp, sizeof(resp));
                        close(sock);
                    }
                }

                // Verify mount succeeded via /proc/self/mountinfo
                FILE *mi = fopen("/proc/self/mountinfo", "r");
                if (mi) {
                    char line[512];
                    while (fgets(line, sizeof(line), mi)) {
                        if (strstr(line, "/proc/cpuinfo") != nullptr) {
                            cpu_mount_active_ = true;
                            break;
                        }
                    }
                    fclose(mi);
                }

                if (cpu_mount_active_) {
                    LOGI("COPG: Target-private CPU mount confirmed active for %s", process_name.c_str());
                } else {
                    LOGW("COPG: CPU mount requested and submitted for %s, but /proc/cpuinfo mount not observed in mountinfo", process_name.c_str());
                }
            } else {
                LOGW("COPG: CPU mount requested for %s, but cpuinfo_spoof file missing at %s", process_name.c_str(), spoof_source.c_str());
            }
        } else {
            LOGW("COPG: CPU mount requested for %s, but target namespace is not isolated (shares inode with PID 1: %lu). Aborting mount to protect system namespace.",
                 process_name.c_str(), (unsigned long)self_st.st_ino);
        }
    }

    // Versioned Runtime Acknowledgement protocol
    RuntimeAck ack;
    ack.engine = "copg";
    ack.processName = process_name;
    ack.pid = getpid();
    ack.uid = getuid();
    ack.configGeneration = applied_generation_;

    if (getInterceptedCount() > 0) {
        ack.eventType = AckEventType::HOOK_OBSERVED;
        ack.initResult = 0;
        ack.detail = "COPG PLT property queries intercepted and spoofed (" + std::to_string(getInterceptedCount()) + " calls)";
    } else if (plt_commit_succeeded_ || current_flags_.needs_device_spoof || cpu_mount_active_) {
        ack.eventType = AckEventType::HOOK_INITIALIZED;
        ack.initResult = 0;
        ack.detail = "COPG hooks initialized (discovered=" + std::to_string(libraries_discovered_) +
                     ", registered=" + std::to_string(hooks_registered_) +
                     ", committed=" + (plt_commit_succeeded_ ? "1" : "0") +
                     ", cpu_mount=" + (cpu_mount_active_ ? "1" : "0") + ")";
    } else {
        ack.eventType = AckEventType::PROCESS_LOADED;
        ack.initResult = 0;
        ack.detail = "COPG targeted process loaded";
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

    // If stealth mode is requested and no persistent native hooks remain, unload
    if (!current_flags_.needs_plt_hook && api_) {
        api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        LOGD("COPG: DLCLOSE_MODULE_LIBRARY requested for stealth");
    }
}

} // namespace kaorios
