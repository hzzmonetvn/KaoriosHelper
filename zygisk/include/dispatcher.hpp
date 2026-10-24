/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#pragma once

#include <vector>
#include <memory>
#include <string>
#include "zygisk.hpp"
#include "engine.hpp"

namespace kaorios {

class KaoriosDispatcher : public zygisk::ModuleBase {
public:
    KaoriosDispatcher();
    virtual ~KaoriosDispatcher() override = default;

    virtual void onLoad(zygisk::Api *api, JNIEnv *env) override;
    virtual void preAppSpecialize(zygisk::AppSpecializeArgs *args) override;
    virtual void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override;
    virtual void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override;
    virtual void postServerSpecialize(const zygisk::ServerSpecializeArgs *args) override;

    // Register engine to dispatcher
    void registerEngine(std::unique_ptr<IEngine> engine);

private:
    std::string getProcessName(JNIEnv *env, jstring j_nice_name);

    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    std::vector<std::unique_ptr<IEngine>> engines_;
    std::vector<IEngine*> active_app_engines_;
    bool handling_server_ = false;
};

} // namespace kaorios
