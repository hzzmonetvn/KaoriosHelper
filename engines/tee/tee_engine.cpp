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

    // Structural XML Validator & Tokenizer
    // Validates balanced element hierarchy, tags, attributes, and certificate structure
    // Note: This performs XML structural validation only, NOT cryptographic validation.
    std::vector<std::string> tag_stack;
    std::string current_key_algorithm;
    bool has_keybox_root = false;
    bool in_device_id = false;
    std::string device_id_buffer;

    size_t i = 0;
    const size_t len = content.size();
    bool xml_error = false;

    while (i < len) {
        if (content[i] == '<') {
            // Check for XML declaration or comment <?xml ... ?> or <!-- ... -->
            if (i + 1 < len && content[i + 1] == '?') {
                size_t close_decl = content.find("?>", i + 2);
                if (close_decl == std::string::npos) { xml_error = true; break; }
                i = close_decl + 2;
                continue;
            }
            if (i + 3 < len && content.substr(i, 4) == "<!--") {
                size_t close_comment = content.find("-->", i + 4);
                if (close_comment == std::string::npos) { xml_error = true; break; }
                i = close_comment + 3;
                continue;
            }

            size_t tag_end = content.find('>', i);
            if (tag_end == std::string::npos) {
                xml_error = true;
                break;
            }

            std::string tag_body = content.substr(i + 1, tag_end - i - 1);
            i = tag_end + 1;

            if (tag_body.empty()) { xml_error = true; break; }

            bool is_closing = (tag_body[0] == '/');
            bool is_self_closing = (!tag_body.empty() && tag_body.back() == '/');

            if (is_closing) {
                std::string closing_name = tag_body.substr(1);
                size_t ws = closing_name.find_first_of(" \t\r\n");
                if (ws != std::string::npos) closing_name = closing_name.substr(0, ws);

                if (tag_stack.empty() || tag_stack.back() != closing_name) {
                    LOGW("TEE: Unbalanced closing XML tag </%s> in keybox.xml", closing_name.c_str());
                    xml_error = true;
                    break;
                }

                if (closing_name == "Key") {
                    current_key_algorithm.clear();
                } else if (closing_name == "DeviceId") {
                    in_device_id = false;
                    info.device_id = device_id_buffer;
                }

                tag_stack.pop_back();
            } else {
                if (is_self_closing) {
                    tag_body.pop_back();
                }

                std::stringstream ss(tag_body);
                std::string tag_name;
                ss >> tag_name;

                if (tag_name == "KeyboxContent" || tag_name == "Keybox") {
                    if (tag_stack.empty()) {
                        has_keybox_root = true;
                    }
                }

                if (tag_name == "Key") {
                    std::string attr;
                    while (ss >> attr) {
                        if (attr.rfind("algorithm=", 0) == 0) {
                            std::string algo = attr.substr(10);
                            if (algo.size() >= 2 && (algo.front() == '"' || algo.front() == '\'')) {
                                algo = algo.substr(1, algo.size() - 2);
                            }
                            std::transform(algo.begin(), algo.end(), algo.begin(), ::tolower);
                            current_key_algorithm = algo;
                            if (algo == "ecdsa" || algo == "ec") info.has_ecdsa = true;
                            if (algo == "rsa") info.has_rsa = true;
                        }
                    }
                } else if (tag_name == "Certificate") {
                    info.cert_count++;
                } else if (tag_name == "DeviceId") {
                    in_device_id = true;
                    device_id_buffer.clear();
                }

                if (!is_self_closing) {
                    tag_stack.push_back(tag_name);
                }
            }
        } else {
            if (in_device_id) {
                if (content[i] != '\r' && content[i] != '\n' && content[i] != '\t' && content[i] != ' ') {
                    device_id_buffer += content[i];
                }
            }
            i++;
        }
    }

    // Structural acceptance: well-formed balanced XML, recognized Keybox root,
    // key algorithm specification, and certificate chain count.
    // This is strictly XML syntax and structural validation, NOT cryptographic chain validation.
    if (!xml_error && tag_stack.empty() && has_keybox_root) {
        if ((info.has_ecdsa || info.has_rsa) && info.cert_count > 0) {
            info.valid = true;
        }
    }

    // Secure zeroization of local content buffer
    std::fill(content.begin(), content.end(), '\0');
    return info;
}

bool TeeEngine::hasValidKeybox() const {
    return parseKeybox().valid;
}

bool TeeEngine::isAttestationSimulationActive() const {
    // TEE runtime status remains INCOMPLETE_RUNTIME.
    // Structural XML parsing does not perform cryptographic attestation or certificate validation.
    // Keystore2 system daemon interception is not bundled.
    return false;
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

    LOGI("TEE: Keybox XML structurally validated for %s (ECDSA=%d, RSA=%d, certs=%zu). "
         "Note: Keystore2 system daemon interception is not bundled; runtime status is INCOMPLETE_RUNTIME.",
         process_name.c_str(), info.has_ecdsa, info.has_rsa, info.cert_count);

    // Note: Keystore2 (/system/bin/keystore2, UID 1017) runs as an isolated system daemon
    // outside Zygote. Structural XML parsing of keybox.xml does NOT perform cryptographic
    // certificate chain validation or root-of-trust verification. Since Keystore2 daemon
    // interception is not bundled, TEE runtime status remains INCOMPLETE_RUNTIME.
}

} // namespace kaorios
