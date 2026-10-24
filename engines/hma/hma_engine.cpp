/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#include "hma_engine.hpp"
#include "logging.hpp"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>
#include <cstring>

namespace kaorios {

HmaEngine::HmaEngine(std::string data_dir) : data_dir_(std::move(data_dir)) {}

HmaEngine::~HmaEngine() {
    cleanupDex();
}

void HmaEngine::cleanupDex() {
    if (dex_entrypoint_ && env_) {
        env_->DeleteGlobalRef(dex_entrypoint_);
        dex_entrypoint_ = nullptr;
    }
}

bool HmaEngine::isEnabled() const {
    std::string disabled_marker = data_dir_ + "/hma.disabled";
    struct stat st{};
    return stat(disabled_marker.c_str(), &st) != 0;
}

bool HmaEngine::shouldHandleServer() {
    return isEnabled();
}

bool HmaEngine::shouldHandleApp(const std::string &process_name, jint uid) {
    // HMA hooks system_server exclusively (UID 1000)
    return false;
}

void HmaEngine::onLoad(zygisk::Api *api, JNIEnv *env) {
    api_ = api;
    env_ = env;
    LOGD("HMA Engine onLoad");
}

bool HmaEngine::loadConfig() {
    std::string config_path = data_dir_ + "/hma.json";
    struct stat st{};
    if (stat(config_path.c_str(), &st) != 0 || st.st_size == 0) {
        LOGW("HMA config missing or empty: %s", config_path.c_str());
        return false;
    }

    std::ifstream file(config_path);
    if (!file.is_open()) {
        LOGE("Failed to open HMA config: %s", config_path.c_str());
        return false;
    }

    std::lock_guard<std::mutex> lock(config_mutex_);
    scope_map_.clear();
    global_hidden_packages_.clear();

    // Default global hidden root/tamper packages
    global_hidden_packages_.insert("com.topjohnwu.magisk");
    global_hidden_packages_.insert("io.github.vvb2060.magisk");
    global_hidden_packages_.insert("me.weishu.kernelsu");
    global_hidden_packages_.insert("org.apatch");
    global_hidden_packages_.insert("org.lsposed.manager");

    std::string line;
    std::string current_caller = "";
    bool in_scope = false;

    while (std::getline(file, line)) {
        // Fast JSON scanner for scope entries
        if (line.find("\"scope\"") != std::string::npos) {
            in_scope = true;
            continue;
        }

        if (in_scope) {
            size_t q1 = line.find('"');
            if (q1 != std::string::npos) {
                size_t q2 = line.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    std::string token = line.substr(q1 + 1, q2 - q1 - 1);
                    if (token.find('.') != std::string::npos) {
                        if (line.find('{') != std::string::npos || line.find(':') != std::string::npos) {
                            current_caller = token;
                            scope_map_[current_caller] = {};
                        } else if (!current_caller.empty()) {
                            scope_map_[current_caller].insert(token);
                        }
                    }
                }
            }
            if (line.find('}') != std::string::npos && line.find('{') == std::string::npos) {
                current_caller.clear();
            }
        }
    }

    last_config_mtime_ = st.st_mtime;
    config_loaded_ = true;
    LOGI("HMA config loaded: %zu scoped callers, %zu global hidden packages", scope_map_.size(), global_hidden_packages_.size());
    return true;
}

bool HmaEngine::reloadConfigIfNeeded() {
    std::string config_path = data_dir_ + "/hma.json";
    struct stat st{};
    if (stat(config_path.c_str(), &st) != 0) {
        return false;
    }

    if (st.st_mtime != last_config_mtime_) {
        LOGI("HMA config mtime changed, reloading...");
        return loadConfig();
    }
    return true;
}

bool HmaEngine::shouldHide(int calling_uid, const std::string &target_pkg) const {
    // Android system_server / root must NEVER be filtered
    if (calling_uid == 0 || calling_uid == 1000) {
        return false;
    }

    std::lock_guard<std::mutex> lock(config_mutex_);
    if (!config_loaded_) {
        return false;
    }

    // Check if target package is in global blacklisted root packages
    if (global_hidden_packages_.count(target_pkg) > 0) {
        filter_count_++;
        return true;
    }

    return false;
}

bool HmaEngine::tryLoadDex(int module_dir, const char *package_name, const char *process_name) {
    if (!env_) return false;

    // Look for classes.dex in module_dir or data_dir
    std::string dex_path = "";
    if (module_dir >= 0) {
        int fd = openat(module_dir, "classes.dex", O_RDONLY);
        if (fd >= 0) {
            close(fd);
            dex_path = "classes.dex";
        }
    }
    if (dex_path.empty()) {
        std::string fallback = data_dir_ + "/classes.dex";
        struct stat st{};
        if (stat(fallback.c_str(), &st) == 0 && st.st_size > 0) {
            dex_path = fallback;
        }
    }

    if (dex_path.empty()) {
        LOGD("HMA: No classes.dex found in module or data dir; skipping DEX injection");
        return false;
    }

    int fd = -1;
    if (module_dir >= 0 && dex_path == "classes.dex") {
        fd = openat(module_dir, dex_path.c_str(), O_RDONLY);
    } else {
        fd = open(dex_path.c_str(), O_RDONLY);
    }

    if (fd < 0) {
        LOGE("HMA: Failed to open DEX file: %s", dex_path.c_str());
        return false;
    }

    struct stat st{};
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
        close(fd);
        return false;
    }

    void *data = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (data == MAP_FAILED) {
        LOGE("HMA: Failed to mmap DEX file");
        return false;
    }

    jclass clClass = env_->FindClass("java/lang/ClassLoader");
    if (!clClass) {
        munmap(data, st.st_size);
        env_->ExceptionClear();
        return false;
    }

    jmethodID getSysLoader = env_->GetStaticMethodID(clClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    jobject sysLoader = env_->CallStaticObjectMethod(clClass, getSysLoader);

    jclass byteBufClass = env_->FindClass("java/nio/ByteBuffer");
    jclass inMemClClass = env_->FindClass("dalvik/system/InMemoryDexClassLoader");

    if (!byteBufClass || !inMemClClass || !sysLoader) {
        munmap(data, st.st_size);
        env_->ExceptionClear();
        return false;
    }

    jobject directBuf = env_->NewDirectByteBuffer(data, st.st_size);
    jobjectArray bufArray = env_->NewObjectArray(1, byteBufClass, directBuf);

    jmethodID inMemInit = env_->GetMethodID(inMemClClass, "<init>", "([Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    jobject inMemLoader = env_->NewObject(inMemClClass, inMemInit, bufArray, sysLoader);

    if (!inMemLoader) {
        env_->ExceptionClear();
        munmap(data, st.st_size);
        return false;
    }

    // Try loading entrypoints
    jmethodID loadClassMid = env_->GetMethodID(clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    const char *entry_names[] = {
        "com.v7878.zygisk.EntryPoint",
        "org.frknkrc44.hma_oss.zygote.ZygoteEntry"
    };

    jclass entryClass = nullptr;
    for (const char *name : entry_names) {
        jstring jname = env_->NewStringUTF(name);
        entryClass = (jclass) env_->CallObjectMethod(inMemLoader, loadClassMid, jname);
        env_->DeleteLocalRef(jname);
        if (entryClass) break;
        env_->ExceptionClear();
    }

    if (entryClass) {
        dex_entrypoint_ = (jclass) env_->NewGlobalRef(entryClass);
        LOGI("HMA: Injected DEX loaded successfully into system_server");
    }

    env_->DeleteLocalRef(directBuf);
    env_->DeleteLocalRef(bufArray);
    env_->DeleteLocalRef(inMemLoader);
    env_->DeleteLocalRef(sysLoader);
    return (dex_entrypoint_ != nullptr);
}

void HmaEngine::preServerSpecialize(zygisk::ServerSpecializeArgs *args) {
    LOGD("HMA preServerSpecialize");
    // Pre-load configuration while still root in Zygote before system_server drops to UID 1000
    config_loaded_ = loadConfig();

    if (api_) {
        int mod_dir = api_->getModuleDir();
        if (mod_dir >= 0) {
            tryLoadDex(mod_dir, "android", "system_server");
            close(mod_dir);
        }
    }
}

void HmaEngine::postServerSpecialize(const zygisk::ServerSpecializeArgs *args) {
    LOGI("HMA initializing inside system_server");
    try {
        if (!config_loaded_) {
            config_loaded_ = loadConfig();
        }

        // If DEX was injected, call its initialization hook
        if (dex_entrypoint_ && env_) {
            jmethodID mainMid = env_->GetStaticMethodID(dex_entrypoint_, "main", "()V");
            if (mainMid) {
                env_->CallStaticVoidMethod(dex_entrypoint_, mainMid);
                LOGI("HMA: ZygoteEntry.main() executed inside system_server");
            } else {
                env_->ExceptionClear();
            }
        }

        LOGI("HMA: In-process PackageManagerService and SettingsProvider protection active");
    } catch (const std::exception &e) {
        LOGE("HMA initialization caught exception: %s. Continuing safely.", e.what());
    } catch (...) {
        LOGE("HMA initialization caught unknown exception. Continuing safely.");
    }
}

} // namespace kaorios
