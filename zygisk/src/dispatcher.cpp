/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#include "dispatcher.hpp"
#include "logging.hpp"

namespace kaorios {

KaoriosDispatcher::KaoriosDispatcher() = default;

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
        if (engine->isEnabled()) {
            engine->onLoad(api, env);
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
        if (engine->isEnabled() && engine->shouldHandleApp(process_name, uid)) {
            active_app_engines_.push_back(engine.get());
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
        engine->preAppSpecialize(args, process_name);
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
        engine->postAppSpecialize(args, process_name);
    }
}

void KaoriosDispatcher::preServerSpecialize(zygisk::ServerSpecializeArgs *args) {
    handling_server_ = false;

    for (auto &engine : engines_) {
        if (engine->isEnabled() && engine->shouldHandleServer()) {
            handling_server_ = true;
            engine->preServerSpecialize(args);
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

    for (auto &engine : engines_) {
        if (engine->isEnabled() && engine->shouldHandleServer()) {
            engine->postServerSpecialize(args);
        }
    }
}

} // namespace kaorios
