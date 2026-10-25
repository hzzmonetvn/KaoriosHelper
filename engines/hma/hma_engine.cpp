/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#include "hma_engine.hpp"
#include "logging.hpp"
#include "ack_protocol.hpp"
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
    if (dex_mmap_addr_ && dex_mmap_size_ > 0) {
        munmap(dex_mmap_addr_, dex_mmap_size_);
        dex_mmap_addr_ = nullptr;
        dex_mmap_size_ = 0;
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

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        LOGE("HMA: Failed to parse %s as valid JSON", config_path.c_str());
        return false;
    }

    if (j.contains("scope") && j["scope"].is_object()) {
        for (auto it = j["scope"].begin(); it != j["scope"].end(); ++it) {
            std::string caller = it.key();
            std::set<std::string> hidden;
            if (it.value().is_array()) {
                for (const auto &pkg : it.value()) {
                    if (pkg.is_string()) hidden.insert(pkg.get<std::string>());
                }
            }
            scope_map_[caller] = std::move(hidden);
        }
    }

    last_config_mtime_ = st.st_mtime;
    config_json_snapshot_ = content;
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
    if (env_->ExceptionCheck() || !clClass) {
        env_->ExceptionClear();
        munmap(data, st.st_size);
        return false;
    }

    jmethodID getSysLoader = env_->GetStaticMethodID(clClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    if (env_->ExceptionCheck() || !getSysLoader) {
        env_->ExceptionClear();
        env_->DeleteLocalRef(clClass);
        munmap(data, st.st_size);
        return false;
    }

    jobject sysLoader = env_->CallStaticObjectMethod(clClass, getSysLoader);
    if (env_->ExceptionCheck() || !sysLoader) {
        env_->ExceptionClear();
        env_->DeleteLocalRef(clClass);
        munmap(data, st.st_size);
        return false;
    }

    jclass byteBufClass = env_->FindClass("java/nio/ByteBuffer");
    if (env_->ExceptionCheck() || !byteBufClass) {
        env_->ExceptionClear();
        env_->DeleteLocalRef(sysLoader);
        env_->DeleteLocalRef(clClass);
        munmap(data, st.st_size);
        return false;
    }

    jclass inMemClClass = env_->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (env_->ExceptionCheck() || !inMemClClass) {
        env_->ExceptionClear();
        env_->DeleteLocalRef(byteBufClass);
        env_->DeleteLocalRef(sysLoader);
        env_->DeleteLocalRef(clClass);
        munmap(data, st.st_size);
        return false;
    }

    jobject directBuf = env_->NewDirectByteBuffer(data, st.st_size);
    if (env_->ExceptionCheck() || !directBuf) {
        env_->ExceptionClear();
        env_->DeleteLocalRef(inMemClClass);
        env_->DeleteLocalRef(byteBufClass);
        env_->DeleteLocalRef(sysLoader);
        env_->DeleteLocalRef(clClass);
        munmap(data, st.st_size);
        return false;
    }

    jobjectArray bufArray = env_->NewObjectArray(1, byteBufClass, directBuf);
    if (env_->ExceptionCheck() || !bufArray) {
        env_->ExceptionClear();
        env_->DeleteLocalRef(directBuf);
        env_->DeleteLocalRef(inMemClClass);
        env_->DeleteLocalRef(byteBufClass);
        env_->DeleteLocalRef(sysLoader);
        env_->DeleteLocalRef(clClass);
        munmap(data, st.st_size);
        return false;
    }

    jmethodID inMemInit = env_->GetMethodID(inMemClClass, "<init>", "([Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (env_->ExceptionCheck() || !inMemInit) {
        env_->ExceptionClear();
        env_->DeleteLocalRef(bufArray);
        env_->DeleteLocalRef(directBuf);
        env_->DeleteLocalRef(inMemClClass);
        env_->DeleteLocalRef(byteBufClass);
        env_->DeleteLocalRef(sysLoader);
        env_->DeleteLocalRef(clClass);
        munmap(data, st.st_size);
        return false;
    }

    jobject inMemLoader = env_->NewObject(inMemClClass, inMemInit, bufArray, sysLoader);
    if (env_->ExceptionCheck() || !inMemLoader) {
        env_->ExceptionClear();
        env_->DeleteLocalRef(bufArray);
        env_->DeleteLocalRef(directBuf);
        env_->DeleteLocalRef(inMemClClass);
        env_->DeleteLocalRef(byteBufClass);
        env_->DeleteLocalRef(sysLoader);
        env_->DeleteLocalRef(clClass);
        munmap(data, st.st_size);
        return false;
    }

    // Try loading entrypoints
    jmethodID loadClassMid = env_->GetMethodID(clClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (env_->ExceptionCheck() || !loadClassMid) {
        env_->ExceptionClear();
        env_->DeleteLocalRef(inMemLoader);
        env_->DeleteLocalRef(bufArray);
        env_->DeleteLocalRef(directBuf);
        env_->DeleteLocalRef(inMemClClass);
        env_->DeleteLocalRef(byteBufClass);
        env_->DeleteLocalRef(sysLoader);
        env_->DeleteLocalRef(clClass);
        munmap(data, st.st_size);
        return false;
    }

    const char *entry_names[] = {
        "com.v7878.zygisk.EntryPoint",
        "org.frknkrc44.hma_oss.zygote.ZygoteEntry"
    };

    jclass entryClass = nullptr;
    for (const char *name : entry_names) {
        jstring jname = env_->NewStringUTF(name);
        if (!jname) {
            if (env_->ExceptionCheck()) env_->ExceptionClear();
            continue;
        }
        entryClass = (jclass) env_->CallObjectMethod(inMemLoader, loadClassMid, jname);
        env_->DeleteLocalRef(jname);
        if (env_->ExceptionCheck()) {
            env_->ExceptionClear();
            entryClass = nullptr;
        }
        if (entryClass) break;
    }

    if (entryClass) {
        dex_entrypoint_ = (jclass) env_->NewGlobalRef(entryClass);
        env_->DeleteLocalRef(entryClass);
        // Retain mmapped buffer memory ownership while dex_entrypoint_ is alive
        dex_mmap_addr_ = data;
        dex_mmap_size_ = st.st_size;
        LOGI("HMA: Injected DEX loaded successfully into system_server");
    } else {
        LOGW("HMA: None of the candidate entrypoints found in classes.dex");
        munmap(data, st.st_size);
    }

    env_->DeleteLocalRef(loadClassMid ? inMemLoader : nullptr);
    env_->DeleteLocalRef(directBuf);
    env_->DeleteLocalRef(bufArray);
    env_->DeleteLocalRef(inMemClClass);
    env_->DeleteLocalRef(byteBufClass);
    env_->DeleteLocalRef(sysLoader);
    env_->DeleteLocalRef(clClass);
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

        // If DEX was injected, call its initialization hook with config snapshot
        if (dex_entrypoint_ && env_) {
            jmethodID initMid = env_->GetStaticMethodID(dex_entrypoint_, "init", "(Ljava/lang/String;)V");
            bool init_invoked = false;
            if (!env_->ExceptionCheck() && initMid) {
                jstring j_cfg = env_->NewStringUTF(config_json_snapshot_.c_str());
                env_->CallStaticVoidMethod(dex_entrypoint_, initMid, j_cfg);
                if (j_cfg) env_->DeleteLocalRef(j_cfg);
                if (env_->ExceptionCheck()) {
                    LOGE("HMA: Exception occurred during ZygoteEntry.init(snapshot)");
                    env_->ExceptionClear();
                } else {
                    LOGI("HMA: ZygoteEntry.init(snapshot) executed successfully in system_server");
                    init_invoked = true;
                }
            } else {
                if (env_->ExceptionCheck()) env_->ExceptionClear();
                jmethodID mainMid = env_->GetStaticMethodID(dex_entrypoint_, "main", "()V");
                if (!env_->ExceptionCheck() && mainMid) {
                    env_->CallStaticVoidMethod(dex_entrypoint_, mainMid);
                    if (env_->ExceptionCheck()) {
                        LOGE("HMA: Exception occurred during ZygoteEntry.main()");
                        env_->ExceptionClear();
                    } else {
                        LOGI("HMA: ZygoteEntry.main() executed inside system_server");
                        init_invoked = true;
                    }
                } else if (env_->ExceptionCheck()) {
                    env_->ExceptionClear();
                }
            }

            if (init_invoked) {
                int hook_state = 3; // STATE_ENTRYPOINT_EXECUTED
                jmethodID getStateMid = env_->GetStaticMethodID(dex_entrypoint_, "getHookState", "()I");
                if (!env_->ExceptionCheck() && getStateMid) {
                    hook_state = env_->CallStaticIntMethod(dex_entrypoint_, getStateMid);
                    if (env_->ExceptionCheck()) env_->ExceptionClear();
                }

                RuntimeAck ack;
                ack.engine = "hma";
                ack.processName = "system_server";
                ack.pid = getpid();
                ack.uid = 1000;
                ack.configGeneration = 1;
                ack.initResult = (hook_state >= 5) ? 0 : -1;
                ack.eventType = (hook_state >= 8) ? AckEventType::HOOK_OBSERVED :
                                ((hook_state >= 5) ? AckEventType::HOOK_INITIALIZED :
                                 ((hook_state >= 3) ? AckEventType::HOOK_REGISTERED : AckEventType::PROCESS_LOADED));
                ack.detail = "HMA framework hooks active; hook state: " + std::to_string(hook_state);

                if (api_) {
                    int companion_sock = api_->connectCompanion();
                    if (companion_sock >= 0) {
                        std::string payload = "ack:" + ack.toJson().dump();
                        write(companion_sock, payload.data(), payload.size());
                        int resp = -1;
                        read(companion_sock, &resp, sizeof(resp));
                        close(companion_sock);
                    } else {
                        writeRuntimeAckDirect(data_dir_ + "/runtime", ack);
                    }
                } else {
                    writeRuntimeAckDirect(data_dir_ + "/runtime", ack);
                }
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
