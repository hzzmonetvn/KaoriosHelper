/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#pragma once

#include <string>
#include <vector>
#include <mutex>
#include "engine.hpp"

namespace kaorios {

struct KeyboxInfo {
    bool valid = false;
    bool has_ecdsa = false;
    bool has_rsa = false;
    size_t cert_count = 0;
    std::string device_id;
};

class TeeEngine : public IEngine {
public:
    explicit TeeEngine(std::string data_dir = "/data/adb/modules/kaorioshelper/data");
    virtual ~TeeEngine() override = default;

    virtual const char* getName() const override { return "tee"; }
    virtual bool isEnabled() const override;

    virtual bool shouldHandleApp(const std::string &process_name, jint uid) override;
    virtual bool shouldHandleServer() override { return false; }

    virtual void onLoad(zygisk::Api *api, JNIEnv *env) override;
    virtual void preAppSpecialize(zygisk::AppSpecializeArgs *args, const std::string &process_name) override;
    virtual void postAppSpecialize(const zygisk::AppSpecializeArgs *args, const std::string &process_name) override;

    // Returns whether valid keybox XML is present
    bool hasValidKeybox() const;

    // Detailed keybox validation and parsing (zeroizes sensitive buffers)
    KeyboxInfo parseKeybox() const;

    // Coexistence: Returns true if KeyMint attestation simulation is ready
    bool isAttestationSimulationActive() const;

private:
    std::string data_dir_;
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    mutable std::mutex keybox_mutex_;
};

} // namespace kaorios
