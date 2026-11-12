/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#include "dispatcher.hpp"
#include "logging.hpp"
#include <algorithm>

namespace kaorios {

KaoriosDispatcher::KaoriosDispatcher() = default;

bool KaoriosDispatcher::failed(IEngine *engine) const {
    return std::find(failed_engines_.begin(), failed_engines_.end(), engine) != failed_engines_.end();
}

void KaoriosDispatcher::recordFailure(IEngine *engine) {
    if (!failed(engine)) failed_engines_.push_back(engine);
    LOGE("Engine %s lifecycle failed; isolated for this process", engine->getName());
}

void KaoriosDispatcher::registerEngine(std::unique_ptr<IEngine> engine) {
    if (engine) {
        LOGI("Registering engine: %s", engine->getName());
        engines_.push_back(std::move(engine));
    }
}

std::string KaoriosDispatcher::getProcessName(JNIEnv *env, jstring j_nice_name) {
    if (!env || !j_nice_name) {
        return "";
    }
    const char *chars = env->GetStringUTFChars(j_nice_name, nullptr);
    if (!chars) {
        return "";
    }
    std::string name(chars);
    env->ReleaseStringUTFChars(j_nice_name, chars);
    return name;
}

void KaoriosDispatcher::onLoad(zygisk::Api *api, JNIEnv *env) {
    api_ = api;
    env_ = env;
    LOGI("KaoriosHelper Zygisk Dispatcher loaded (API v%d)", ZYGISK_API_VERSION);

    for (auto &engine : engines_) {
        try {
            if (!failed(engine.get()) && engine->isEnabled()) engine->onLoad(api, env);
        } catch (...) {
            recordFailure(engine.get());
        }
    }
}

void KaoriosDispatcher::preAppSpecialize(zygisk::AppSpecializeArgs *args) {
    active_app_engines_.clear();

    std::string process_name = getProcessName(env_, args->nice_name);
    if (process_name.empty() && args->app_data_dir) {
        std::string data_dir = getProcessName(env_, args->app_data_dir);
        size_t last_slash = data_dir.rfind('/');
        if (last_slash != std::string::npos && last_slash + 1 < data_dir.size()) {
            process_name = data_dir.substr(last_slash + 1);
        }
    }
    jint uid = args->uid;

    for (auto &engine : engines_) {
        try {
            if (!failed(engine.get()) && engine->isEnabled() && engine->shouldHandleApp(process_name, uid)) {
                active_app_engines_.push_back(engine.get());
            }
        } catch (...) {
            recordFailure(engine.get());
        }
    }

    if (active_app_engines_.empty()) {
        // No engine requires injection in this process. Unload immediately to preserve RAM.
        if (api_) {
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
        return;
    }

    LOGD("Process '%s' claimed by %zu engine(s)", process_name.c_str(), active_app_engines_.size());
    for (auto *engine : active_app_engines_) {
        try { engine->preAppSpecialize(args, process_name); }
        catch (...) { recordFailure(engine); }
    }
}

void KaoriosDispatcher::postAppSpecialize(const zygisk::AppSpecializeArgs *args) {
    if (active_app_engines_.empty()) {
        return;
    }

    std::string process_name = getProcessName(env_, args->nice_name);
    if (process_name.empty() && args->app_data_dir) {
        std::string data_dir = getProcessName(env_, args->app_data_dir);
        size_t last_slash = data_dir.rfind('/');
        if (last_slash != std::string::npos && last_slash + 1 < data_dir.size()) {
            process_name = data_dir.substr(last_slash + 1);
        }
    }
    for (auto *engine : active_app_engines_) {
        if (failed(engine)) continue;
        try { engine->postAppSpecialize(args, process_name); }
        catch (...) { recordFailure(engine); }
    }
}

void KaoriosDispatcher::preServerSpecialize(zygisk::ServerSpecializeArgs *args) {
    handling_server_ = false;
    active_server_engines_.clear();

    for (auto &engine : engines_) {
        try {
            if (!failed(engine.get()) && engine->isEnabled() && engine->shouldHandleServer()) {
                engine->preServerSpecialize(args);
                active_server_engines_.push_back(engine.get());
                handling_server_ = true;
            }
        } catch (...) {
            recordFailure(engine.get());
        }
    }

    if (!handling_server_) {
        if (api_) {
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }
}

void KaoriosDispatcher::postServerSpecialize(const zygisk::ServerSpecializeArgs *args) {
    if (!handling_server_) {
        return;
    }

    for (auto *engine : active_server_engines_) {
        if (failed(engine)) continue;
        try { engine->postServerSpecialize(args); }
        catch (...) { recordFailure(engine); }
    }
}

} // namespace kaorios
