/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#include "tee_engine.hpp"
#include "logging.hpp"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>

namespace kaorios {

TeeEngine::TeeEngine(std::string data_dir) : data_dir_(std::move(data_dir)) {}

bool TeeEngine::isEnabled() const {
    std::string enabled_marker = data_dir_ + "/tee.enabled";
    struct stat st{};
    return stat(enabled_marker.c_str(), &st) == 0;
}

KeyboxInfo TeeEngine::parseKeybox() const {
    std::lock_guard<std::mutex> lock(keybox_mutex_);
    KeyboxInfo info;
    info.valid = false;

    std::string keybox_path = data_dir_ + "/keybox.xml";
    struct stat st{};
    if (stat(keybox_path.c_str(), &st) != 0 || st.st_size == 0) {
        return info;
    }

    std::ifstream file(keybox_path);
    if (!file.is_open()) {
        return info;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Fast XML tag scanner
    bool has_keybox_root = (content.find("<KeyboxContent") != std::string::npos ||
                            content.find("<Keybox") != std::string::npos);
    if (!has_keybox_root) {
        LOGW("TEE: keybox.xml missing root <KeyboxContent> element");
        return info;
    }

    if (content.find("algorithm=\"ecdsa\"") != std::string::npos ||
        content.find("algorithm=\"ec\"") != std::string::npos ||
        content.find("EC PRIVATE KEY") != std::string::npos) {
        info.has_ecdsa = true;
    }

    if (content.find("algorithm=\"rsa\"") != std::string::npos ||
        content.find("RSA PRIVATE KEY") != std::string::npos) {
        info.has_rsa = true;
    }

    // Count certificates
    size_t pos = 0;
    while ((pos = content.find("<Certificate", pos)) != std::string::npos) {
        info.cert_count++;
        pos += 12;
    }

    // Check for DeviceID
    size_t id_start = content.find("<DeviceId>");
    if (id_start != std::string::npos) {
        size_t id_end = content.find("</DeviceId>", id_start);
        if (id_end != std::string::npos) {
            info.device_id = content.substr(id_start + 10, id_end - id_start - 10);
        }
    }

    // Must have at least one valid key type and at least one certificate in chain
    if ((info.has_ecdsa || info.has_rsa) && info.cert_count > 0) {
        info.valid = true;
    }

    // Secure zeroization of local content buffer
    std::fill(content.begin(), content.end(), '\0');
    return info;
}

bool TeeEngine::hasValidKeybox() const {
    return parseKeybox().valid;
}

bool TeeEngine::isAttestationSimulationActive() const {
    if (!isEnabled()) return false;
    return hasValidKeybox();
}

bool TeeEngine::shouldHandleApp(const std::string &process_name, jint uid) {
    if (!isEnabled()) {
        return false;
    }

    // Attestation simulation applies to GMS unstable (DroidGuard) and apps requesting KeyMint attestation
    if (process_name == "com.google.android.gms.unstable" ||
        process_name == "com.google.android.gms") {
        return true;
    }

    return false;
}

void TeeEngine::onLoad(zygisk::Api *api, JNIEnv *env) {
    api_ = api;
    env_ = env;
    LOGD("TEE Engine onLoad");
}

void TeeEngine::preAppSpecialize(zygisk::AppSpecializeArgs *args, const std::string &process_name) {
    LOGI("TEE preAppSpecialize for process: %s", process_name.c_str());
}

void TeeEngine::postAppSpecialize(const zygisk::AppSpecializeArgs *args, const std::string &process_name) {
    auto info = parseKeybox();
    if (!info.valid) {
        LOGW("TEE: Keybox XML missing or invalid in %s. Hardware attestation simulation inactive.", data_dir_.c_str());
        return;
    }

    LOGI("TEE: Hardware KeyStore / KeyMint attestation simulation active for %s (ECDSA=%d, RSA=%d, certs=%zu)",
         process_name.c_str(), info.has_ecdsa, info.has_rsa, info.cert_count);

    // Note: Keystore2 (/system/bin/keystore2, UID 1017) runs as an isolated system daemon
    // outside Zygote. Hardware attestation certificates are provided either via the background
    // TEE supervisor daemon or client-side Binder interception.
}

} // namespace kaorios
