/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * KaoriosHelper Native Control CLI (helperctl)
 */

#include <iostream>
#include "../../core/config/native_store.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <cstring>
#include <cerrno>
#include "../../third_party/json/nlohmann/json.hpp"
#if __has_include(<ack_protocol.hpp>)
#include <ack_protocol.hpp>
#elif __has_include("../../include/ack_protocol.hpp")
#include "../../include/ack_protocol.hpp"
#endif
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <time.h>

namespace fs = std::filesystem;

class HelperCtl {
public:
    HelperCtl(std::string mod_dir = "") {
        const char *env_mod = getenv("KAORIOS_MOD_DIR");
        if (env_mod && env_mod[0] != '\0') {
            mod_dir_ = env_mod;
        } else if (!mod_dir.empty()) {
            mod_dir_ = mod_dir;
        } else {
            // Determine mod_dir from executable path or default /data/adb/modules/kaorioshelper
            char self_path[PATH_MAX] = {0};
            ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
            if (len > 0) {
                self_path[len] = '\0';
                fs::path p(self_path);
                if (p.has_parent_path()) {
                    mod_dir_ = p.parent_path().string();
                }
            }
            std::error_code ec;
            if (mod_dir_.empty() || !fs::exists(mod_dir_ + "/module.prop", ec)) {
                mod_dir_ = "/data/adb/modules/kaorioshelper";
            }
        }
        const char *env_data = getenv("KAORIOS_DATA_DIR");
        if (env_data && env_data[0] != '\0') {
            data_dir_ = env_data;
        } else {
            data_dir_ = mod_dir_ + "/data";
        }
    }

    void setDataDir(const std::string &dir) {
        data_dir_ = dir;
    }

    std::string getVersion() {
        std::string prop_file = mod_dir_ + "/module.prop";
        std::ifstream f(prop_file);
        if (f.is_open()) {
            std::string line;
            while (std::getline(f, line)) {
                if (line.rfind("version=", 0) == 0) {
                    return line.substr(8);
                }
            }
        }
        return "v1.0.0";
    }

    bool isModuleDisabled() {
        struct stat st{};
        return (stat((mod_dir_ + "/disable").c_str(), &st) == 0);
    }

    // Comprehensive Engine Status containing all 16 architectural fields
    struct EngineStatus {
        std::string name;
        bool installed{true};
        bool enabled{false};
        bool configValid{false};
        bool runtimeAvailable{false};
        bool processLoaded{false};
        bool hookRegistered{false};
        std::string hookInitialized{"NOT_INITIALIZED"};
        bool runtimeObserved{false};
        bool deviceVerified{false};
        uint64_t storedGeneration{0};
        uint64_t requestedGeneration{0};
        uint64_t appliedGeneration{0};
        std::string lastError{""};
        std::string lastRuntimeEvent{"NONE"};
        uint64_t lastRuntimeTimestamp{0};
        uint64_t lastVerifiedTimestamp{0};
        bool pendingReload{false};
        std::string status{"DISABLED"};
        std::string details{""};
    };

    uint64_t getStoredGeneration(const std::string &engine) {
        std::string gen_file = data_dir_ + "/" + engine + ".stored_gen";
        std::ifstream f(gen_file);
        if (f.is_open()) {
            uint64_t gen = 0;
            if (f >> gen) return gen;
        }
        // Default generation is 1 if configuration exists, 0 otherwise
        std::string cfg_name = (engine == "tee") ? "keybox.xml" : ((engine == "copg") ? "spoof.json" : (engine + ".json"));
        struct stat st{};
        if (stat((data_dir_ + "/" + cfg_name).c_str(), &st) == 0 && st.st_size > 0) {
            return 1;
        }
        return 0;
    }

    void setStoredGeneration(const std::string &engine, uint64_t gen) {
        std::string gen_file = data_dir_ + "/" + engine + ".stored_gen";
        std::ofstream f(gen_file);
        if (f.is_open()) f << gen << "\n";
    }

    uint64_t getRequestedGeneration(const std::string &engine) {
        std::string gen_file = data_dir_ + "/" + engine + ".requested_gen";
        std::ifstream f(gen_file);
        if (f.is_open()) {
            uint64_t gen = 0;
            if (f >> gen) return gen;
        }
        return 0; // Absence is not evidence that a reload was requested.
    }

    void setRequestedGeneration(const std::string &engine, uint64_t gen) {
        std::string gen_file = data_dir_ + "/" + engine + ".requested_gen";
        std::ofstream f(gen_file);
        if (f.is_open()) f << gen << "\n";
    }

    uint64_t getVerifiedTimestamp(const std::string &engine) {
        std::string vfile = data_dir_ + "/" + engine + ".verified";
        struct stat st{};
        if (stat(vfile.c_str(), &st) == 0) {
            std::ifstream f(vfile);
            uint64_t ts = 0;
            if (f >> ts && ts > 0) return ts;
            return static_cast<uint64_t>(st.st_mtime) * 1000ULL;
        }
        return 0;
    }

    void setVerified(const std::string &engine, bool verified) {
        std::string vfile = data_dir_ + "/" + engine + ".verified";
        if (verified) {
            struct timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
            std::ofstream f(vfile);
            if (f.is_open()) f << now_ms << "\n";
        } else {
            unlink(vfile.c_str());
        }
    }

    std::map<std::string, EngineStatus> getEngineStatuses() {
        std::map<std::string, EngineStatus> res;
        struct stat st{};

        // ── HMA ─────────────────────────────────────────────────────────────
        EngineStatus hma;
        hma.name = "hma";
        hma.installed = true;
        hma.enabled = (stat((data_dir_ + "/hma.disabled").c_str(), &st) != 0);
        hma.pendingReload = (stat((data_dir_ + "/hma.reload_pending").c_str(), &st) == 0);
        hma.storedGeneration = getStoredGeneration("hma");
        hma.requestedGeneration = getRequestedGeneration("hma");
        hma.appliedGeneration = 0;
        hma.lastVerifiedTimestamp = getVerifiedTimestamp("hma");
        hma.deviceVerified = (hma.lastVerifiedTimestamp > 0);

        // Check classes.dex
        bool dex_exists = (stat((data_dir_ + "/classes.dex").c_str(), &st) == 0 && st.st_size > 0) ||
                          (stat((mod_dir_ + "/classes.dex").c_str(), &st) == 0 && st.st_size > 0);
        hma.runtimeAvailable = dex_exists;

        // Check hma.json
        if (stat((data_dir_ + "/hma.json").c_str(), &st) == 0 && st.st_size > 0) {
            std::ifstream f(data_dir_ + "/hma.json");
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            hma.configValid = kaorios::validateNativeJson("hma", content);
            if (!hma.configValid) hma.lastError = "hma.json malformed JSON syntax";
        } else {
            hma.configValid = false;
            hma.lastError = "hma.json configuration missing or empty";
        }

        kaorios::RuntimeAck hma_ack;
        bool has_hma_ack = kaorios::readRuntimeAck(data_dir_ + "/runtime", "hma", hma_ack);
        if (has_hma_ack) {
            hma.appliedGeneration = hma_ack.configGeneration;
            hma.lastRuntimeEvent = kaorios::ackEventTypeToString(hma_ack.eventType);
            hma.lastRuntimeTimestamp = hma_ack.timestamp;
            hma.processLoaded = (hma_ack.eventType >= kaorios::AckEventType::PROCESS_LOADED);
            hma.hookRegistered = (hma_ack.eventType >= kaorios::AckEventType::HOOK_REGISTERED);
            hma.runtimeObserved = (hma_ack.eventType == kaorios::AckEventType::HOOK_OBSERVED);
        }

        if (!hma.enabled) {
            hma.status = "DISABLED";
            hma.hookInitialized = "DISABLED";
            hma.lastError = "Engine explicitly disabled via hma.disabled marker";
            hma.details = "In-process system_server package protection (disabled)";
        } else if (!hma.configValid) {
            hma.status = "NOT_CONFIGURED";
            hma.hookInitialized = "NOT_INITIALIZED";
            hma.details = "hma.json configuration missing or malformed";
        } else if (!hma.runtimeAvailable) {
            hma.status = "INCOMPLETE_RUNTIME";
            hma.hookInitialized = "INCOMPLETE_RUNTIME";
            hma.lastError = "classes.dex missing: upstream HMA DEX payload not bundled";
            hma.details = "system_server in-memory DEX loader waiting for classes.dex";
        } else if (hma.pendingReload) {
            hma.status = "PENDING_RELOAD";
            hma.hookInitialized = (has_hma_ack && hma_ack.eventType >= kaorios::AckEventType::HOOK_INITIALIZED) ? "INITIALIZED" : "NOT_VERIFIED";
            hma.details = "HMA DEX present; pending system_server restart";
        } else if (has_hma_ack) {
            if (hma_ack.eventType == kaorios::AckEventType::HOOK_OBSERVED) {
                hma.status = "ACTIVE";
                hma.hookInitialized = "INITIALIZED";
                hma.lastError = "";
                hma.details = "system_server framework-level package query filtering observed at runtime";
            } else if (hma_ack.eventType == kaorios::AckEventType::HOOK_INITIALIZED) {
                hma.status = "INITIALIZED";
                hma.hookInitialized = "INITIALIZED";
                hma.lastError = "";
                hma.details = "system_server HMA hooks initialized; awaiting query interception observation";
            } else if (hma_ack.eventType == kaorios::AckEventType::HOOK_REGISTERED) {
                hma.status = "NOT_VERIFIED";
                hma.hookInitialized = "NOT_INITIALIZED";
                hma.details = "system_server HMA hooks registered; awaiting specialization";
            } else if (hma_ack.eventType == kaorios::AckEventType::PROCESS_LOADED) {
                hma.status = "NOT_VERIFIED";
                hma.hookInitialized = "NOT_INITIALIZED";
                hma.details = "system_server process loaded; awaiting hook execution";
            } else {
                hma.status = "ERROR";
                hma.hookInitialized = "FAILED";
                hma.lastError = hma_ack.detail;
                hma.details = "HMA hook failure: " + hma_ack.detail;
            }
        } else {
            hma.status = "NOT_VERIFIED";
            hma.hookInitialized = "NOT_INITIALIZED";
            hma.lastError = "Runtime hook acknowledgement pending system_server specialization";
            hma.details = "HMA configured with DEX; awaiting system_server execution";
        }
        res["hma"] = hma;

        // ── PIF ─────────────────────────────────────────────────────────────
        EngineStatus pif;
        pif.name = "pif";
        pif.installed = true;
        pif.enabled = (stat((data_dir_ + "/pif.disabled").c_str(), &st) != 0);
        pif.pendingReload = (stat((data_dir_ + "/pif.reload_pending").c_str(), &st) == 0);
        pif.storedGeneration = getStoredGeneration("pif");
        pif.requestedGeneration = getRequestedGeneration("pif");
        pif.appliedGeneration = 0;
        pif.lastVerifiedTimestamp = getVerifiedTimestamp("pif");
        pif.deviceVerified = (pif.lastVerifiedTimestamp > 0);
        pif.runtimeAvailable = true;

        if (stat((data_dir_ + "/pif.json").c_str(), &st) == 0 && st.st_size > 0) {
            std::ifstream f(data_dir_ + "/pif.json");
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            pif.configValid = kaorios::validateNativeJson("pif", content);
            if (!pif.configValid) pif.lastError = "pif.json malformed JSON syntax";
        } else {
            pif.configValid = false;
            pif.lastError = "pif.json configuration missing or empty";
        }

        kaorios::RuntimeAck pif_ack;
        bool has_pif_ack = kaorios::readRuntimeAck(data_dir_ + "/runtime", "pif", pif_ack);
        if (has_pif_ack) {
            pif.appliedGeneration = pif_ack.configGeneration;
            pif.lastRuntimeEvent = kaorios::ackEventTypeToString(pif_ack.eventType);
            pif.lastRuntimeTimestamp = pif_ack.timestamp;
            pif.processLoaded = (pif_ack.eventType >= kaorios::AckEventType::PROCESS_LOADED);
            pif.hookRegistered = (pif_ack.eventType >= kaorios::AckEventType::HOOK_REGISTERED);
            pif.runtimeObserved = (pif_ack.eventType == kaorios::AckEventType::HOOK_OBSERVED);
        }

        if (!pif.enabled) {
            pif.status = "DISABLED";
            pif.hookInitialized = "DISABLED";
            pif.lastError = "Engine explicitly disabled via pif.disabled marker";
            pif.details = "Play Integrity Fix property spoofing (disabled)";
        } else if (!pif.configValid) {
            pif.status = "NOT_CONFIGURED";
            pif.hookInitialized = "NOT_INITIALIZED";
            pif.details = "pif.json configuration missing or malformed";
        } else if (pif.pendingReload) {
            pif.status = "PENDING_RELOAD";
            pif.hookInitialized = (has_pif_ack && pif_ack.eventType >= kaorios::AckEventType::HOOK_INITIALIZED) ? "INITIALIZED" : "NOT_VERIFIED";
            pif.details = "PIF configuration updated; pending target reload";
        } else if (has_pif_ack) {
            if (pif_ack.eventType == kaorios::AckEventType::HOOK_OBSERVED) {
                pif.status = "ACTIVE";
                pif.hookInitialized = "INITIALIZED";
                pif.lastError = "";
                pif.details = "ByteDance ShadowHook native libc property hook active and property queries observed";
            } else if (pif_ack.eventType == kaorios::AckEventType::HOOK_INITIALIZED) {
                pif.status = "INITIALIZED";
                pif.hookInitialized = "INITIALIZED";
                pif.lastError = "";
                pif.details = "ByteDance ShadowHook native libc hook registered; awaiting property query observation";
            } else if (pif_ack.eventType == kaorios::AckEventType::HOOK_REGISTERED) {
                pif.status = "NOT_VERIFIED";
                pif.hookInitialized = "NOT_INITIALIZED";
                pif.details = "PIF hook registered in target process; awaiting specialization";
            } else if (pif_ack.eventType == kaorios::AckEventType::PROCESS_LOADED) {
                pif.status = "NOT_VERIFIED";
                pif.hookInitialized = "NOT_INITIALIZED";
                pif.details = "Target process loaded; awaiting hook execution";
            } else {
                pif.status = "ERROR";
                pif.hookInitialized = "FAILED";
                pif.lastError = pif_ack.detail;
                pif.details = "PIF hook failure: " + pif_ack.detail;
            }
        } else {
            pif.status = "NOT_VERIFIED";
            pif.hookInitialized = "NOT_INITIALIZED";
            pif.lastError = "Runtime hook acknowledgement pending target process specialization";
            pif.details = "ByteDance ShadowHook property interceptor configured";
        }
        res["pif"] = pif;

        // ── TEE ─────────────────────────────────────────────────────────────
        EngineStatus tee;
        tee.name = "tee";
        tee.installed = true;
        tee.enabled = (stat((data_dir_ + "/tee.enabled").c_str(), &st) == 0);
        tee.pendingReload = (stat((data_dir_ + "/tee.reload_pending").c_str(), &st) == 0);
        tee.storedGeneration = getStoredGeneration("tee");
        tee.requestedGeneration = getRequestedGeneration("tee");
        tee.appliedGeneration = 0;
        tee.lastVerifiedTimestamp = 0;
        tee.deviceVerified = false;
        tee.runtimeAvailable = false; // Keystore2 ptrace/AIDL integration is unbundled
        tee.runtimeObserved = false;
        tee.processLoaded = false;
        tee.hookRegistered = false;

        if (stat((data_dir_ + "/keybox.xml").c_str(), &st) == 0 && st.st_size > 0) {
            std::ifstream f(data_dir_ + "/keybox.xml");
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            tee.configValid = validateXmlSyntax(content);
            if (!tee.configValid) tee.lastError = "keybox.xml invalid XML structure";
        } else {
            tee.configValid = false;
            tee.lastError = "keybox.xml missing or empty";
        }

        if (!tee.enabled) {
            tee.status = "DISABLED";
            tee.hookInitialized = "DISABLED";
            tee.lastError = "Keystore2 system daemon interception runtime unavailable; engine disabled";
            tee.details = "Hardware Keystore2 / KeyMint attestation simulation (disabled)";
        } else if (!tee.configValid) {
            tee.status = "NOT_CONFIGURED";
            tee.hookInitialized = "INCOMPLETE_RUNTIME";
            tee.details = "tee.enabled active but keybox.xml missing or invalid";
        } else {
            tee.status = "INCOMPLETE_RUNTIME";
            tee.hookInitialized = "INCOMPLETE_RUNTIME";
            tee.lastError = "Keystore2 system daemon interception runtime unavailable";
            tee.details = "Keybox XML structure validated, but Keystore2 daemon ptrace/AIDL is not bundled";
        }
        res["tee"] = tee;

        // ── COPG ────────────────────────────────────────────────────────────
        EngineStatus copg;
        copg.name = "copg";
        copg.installed = true;
        copg.enabled = (stat((data_dir_ + "/copg.enabled").c_str(), &st) == 0);
        copg.pendingReload = (stat((data_dir_ + "/copg.reload_pending").c_str(), &st) == 0);
        copg.storedGeneration = getStoredGeneration("copg");
        copg.requestedGeneration = getRequestedGeneration("copg");
        copg.appliedGeneration = 0;
        copg.lastVerifiedTimestamp = getVerifiedTimestamp("copg");
        copg.deviceVerified = (copg.lastVerifiedTimestamp > 0);
        copg.runtimeAvailable = true;

        std::string copg_file = data_dir_ + "/spoof.json";
        if (stat(copg_file.c_str(), &st) != 0 || st.st_size == 0) {
            copg_file = data_dir_ + "/COPG.json";
        }
        if (stat(copg_file.c_str(), &st) == 0 && st.st_size > 0) {
            std::ifstream f(copg_file);
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            copg.configValid = kaorios::validateNativeJson("copg", content);
            if (!copg.configValid) copg.lastError = "spoof.json malformed JSON syntax";
        } else {
            copg.configValid = false;
            copg.lastError = "spoof.json missing or empty";
        }

        kaorios::RuntimeAck copg_ack;
        bool has_copg_ack = kaorios::readRuntimeAck(data_dir_ + "/runtime", "copg", copg_ack);
        if (has_copg_ack) {
            copg.appliedGeneration = copg_ack.configGeneration;
            copg.lastRuntimeEvent = kaorios::ackEventTypeToString(copg_ack.eventType);
            copg.lastRuntimeTimestamp = copg_ack.timestamp;
            copg.processLoaded = (copg_ack.eventType >= kaorios::AckEventType::PROCESS_LOADED);
            copg.hookRegistered = (copg_ack.eventType >= kaorios::AckEventType::HOOK_REGISTERED);
            copg.runtimeObserved = (copg_ack.eventType == kaorios::AckEventType::HOOK_OBSERVED);
        }

        if (!copg.enabled) {
            copg.status = "DISABLED";
            copg.hookInitialized = "DISABLED";
            copg.lastError = "Engine disabled (copg.enabled marker absent)";
            copg.details = "Game profile and device identity virtualization (disabled)";
        } else if (!copg.configValid) {
            copg.status = "NOT_CONFIGURED";
            copg.hookInitialized = "NOT_INITIALIZED";
            copg.details = "copg.enabled active but spoof.json missing or empty";
        } else if (copg.pendingReload) {
            copg.status = "PENDING_RELOAD";
            copg.hookInitialized = (has_copg_ack && copg_ack.eventType >= kaorios::AckEventType::HOOK_INITIALIZED) ? "INITIALIZED" : "NOT_VERIFIED";
            copg.details = "COPG configuration updated; pending game reload";
        } else if (has_copg_ack) {
            if (copg_ack.eventType == kaorios::AckEventType::HOOK_OBSERVED) {
                copg.status = "ACTIVE";
                copg.hookInitialized = "INITIALIZED";
                copg.lastError = "";
                copg.details = "Per-app Build and PLT property spoofing active and verified in target runtime";
            } else if (copg_ack.eventType == kaorios::AckEventType::HOOK_INITIALIZED) {
                copg.status = "INITIALIZED";
                copg.hookInitialized = "INITIALIZED";
                copg.lastError = "";
                copg.details = "COPG hooks initialized; awaiting on-device target queries";
            } else if (copg_ack.eventType == kaorios::AckEventType::HOOK_REGISTERED) {
                copg.status = "NOT_VERIFIED";
                copg.hookInitialized = "NOT_INITIALIZED";
                copg.details = "COPG hooks registered; awaiting specialization";
            } else if (copg_ack.eventType == kaorios::AckEventType::PROCESS_LOADED) {
                copg.status = "NOT_VERIFIED";
                copg.hookInitialized = "NOT_INITIALIZED";
                copg.details = "Target process loaded; awaiting hook execution";
            } else {
                copg.status = "ERROR";
                copg.hookInitialized = "FAILED";
                copg.lastError = copg_ack.detail;
                copg.details = "COPG hook failure: " + copg_ack.detail;
            }
        } else {
            copg.status = "NOT_VERIFIED";
            copg.hookInitialized = "NOT_INITIALIZED";
            copg.lastError = "Runtime hook acknowledgement pending target app specialization";
            copg.details = "Game profile and device identity virtualization configured";
        }
        res["copg"] = copg;

        // Runtime evidence is not external device verification. Legacy marker files
        // deliberately confer no trust. Only current, successful ACKs imply progress.
        for (auto &[name, s] : res) {
            s.deviceVerified = false;
            s.lastVerifiedTimestamp = 0;
            kaorios::RuntimeAck ack;
            bool hasAck = kaorios::readRuntimeAck(data_dir_ + "/runtime", name, ack);
            bool current = hasAck && ack.engine == name && ack.configGeneration == s.storedGeneration &&
                ack.configGeneration == s.requestedGeneration && s.storedGeneration > 0;
            using E = kaorios::AckEventType;
            bool initialized = current && ack.initResult == 0 && ack.errorCode == 0 &&
                (ack.eventType == E::HOOK_INITIALIZED || ack.eventType == E::HOOK_OBSERVED);
            s.processLoaded = current && ack.eventType != E::APPLY_FAILED && ack.initResult == 0 && ack.errorCode == 0;
            s.hookRegistered = s.processLoaded && (ack.eventType == E::HOOK_REGISTERED || initialized);
            s.runtimeObserved = initialized && ack.eventType == E::HOOK_OBSERVED;
            s.appliedGeneration = initialized ? ack.configGeneration : 0;
            if (name == "tee" || !s.enabled || !s.configValid || !s.runtimeAvailable) continue;
            s.hookInitialized = initialized ? "INITIALIZED" : "NOT_INITIALIZED";
            s.pendingReload = !initialized;
            if (current && (ack.eventType == E::APPLY_FAILED || ack.initResult != 0 || ack.errorCode != 0)) {
                s.status = "FAILED"; s.hookInitialized = "FAILED"; s.lastError = ack.detail;
            } else if (initialized) {
                s.status = s.runtimeObserved ? "DEVICE_UNVERIFIED" : "READY"; s.lastError.clear();
            } else {
                s.status = "PENDING_RELOAD";
                s.details = hasAck && ack.engine == name &&
                    (ack.configGeneration != s.storedGeneration || ack.configGeneration != s.requestedGeneration)
                    ? "Runtime snapshot-generation acknowledgement unsupported by current engine"
                    : "Awaiting successful runtime ACK for stored/requested generation";
            }
        }
        return res;
    }

    bool updateStatusJson() {
        std::error_code ec;
        fs::create_directories(data_dir_, ec);
        if (ec) return false;
        std::string tmp_path = data_dir_ + "/status.json.tmp." + std::to_string(getpid());
        std::string dest_path = data_dir_ + "/status.json";

        std::ofstream f(tmp_path);
        if (!f.is_open()) return false;

        auto engines = getEngineStatuses();
        std::string ver = getVersion();
        bool mod_dis = isModuleDisabled();

        nlohmann::json j;
        j["schemaVersion"] = 1;
        j["moduleVersion"] = ver;
        j["moduleState"] = mod_dis ? "DISABLED" : "ACTIVE";
        j["engines"] = nlohmann::json::object();

        for (const auto &[name, eng] : engines) {
            nlohmann::json ej;
            ej["installed"] = eng.installed;
            ej["enabled"] = eng.enabled;
            ej["configValid"] = eng.configValid;
            ej["runtimeAvailable"] = eng.runtimeAvailable;
            ej["processLoaded"] = eng.processLoaded;
            ej["hookRegistered"] = eng.hookRegistered;
            ej["hookInitialized"] = eng.hookInitialized;
            ej["runtimeObserved"] = eng.runtimeObserved;
            ej["deviceVerified"] = eng.deviceVerified;
            ej["storedGeneration"] = eng.storedGeneration;
            ej["requestedGeneration"] = eng.requestedGeneration;
            ej["appliedGeneration"] = eng.appliedGeneration;
            ej["lastError"] = eng.lastError;
            ej["lastRuntimeEvent"] = eng.lastRuntimeEvent;
            ej["lastRuntimeTimestamp"] = eng.lastRuntimeTimestamp;
            ej["lastVerifiedTimestamp"] = eng.lastVerifiedTimestamp;
            ej["pendingReload"] = eng.pendingReload;
            ej["status"] = eng.status;
            ej["details"] = eng.details;
            j["engines"][name] = ej;
        }

        f << j.dump(2) << "\n";
        f.flush();
        f.close();

        chmod(tmp_path.c_str(), 0600);
        return (rename(tmp_path.c_str(), dest_path.c_str()) == 0);
    }

    int cmdStatus(bool json_mode) {
        auto engines = getEngineStatuses();
        std::string ver = getVersion();
        bool mod_dis = isModuleDisabled();

        if (json_mode) {
            nlohmann::json j;
            j["schemaVersion"] = 1;
            j["module"] = {
                {"id", "kaorioshelper"},
                {"version", ver},
                {"active", !mod_dis}
            };
            j["engines"] = nlohmann::json::object();
            for (const auto &[name, eng] : engines) {
                nlohmann::json ej;
                ej["installed"] = eng.installed;
                ej["enabled"] = eng.enabled;
                ej["configValid"] = eng.configValid;
                ej["runtimeAvailable"] = eng.runtimeAvailable;
                ej["processLoaded"] = eng.processLoaded;
                ej["hookRegistered"] = eng.hookRegistered;
                ej["hookInitialized"] = eng.hookInitialized;
                ej["runtimeObserved"] = eng.runtimeObserved;
                ej["deviceVerified"] = eng.deviceVerified;
                ej["storedGeneration"] = eng.storedGeneration;
                ej["requestedGeneration"] = eng.requestedGeneration;
                ej["appliedGeneration"] = eng.appliedGeneration;
                ej["lastError"] = eng.lastError;
                ej["lastRuntimeEvent"] = eng.lastRuntimeEvent;
                ej["lastRuntimeTimestamp"] = eng.lastRuntimeTimestamp;
                ej["lastVerifiedTimestamp"] = eng.lastVerifiedTimestamp;
                ej["pendingReload"] = eng.pendingReload;
                ej["status"] = eng.status;
                ej["details"] = eng.details;
                j["engines"][name] = ej;
            }
            std::cout << j.dump(2) << "\n";
            return 0;
        }

        std::cout << "===============================================\n";
        std::cout << "         KaoriosHelper Runtime Status          \n";
        std::cout << "===============================================\n";
        std::cout << "Module Version: " << ver << "\n";
        std::cout << "Module State:   " << (mod_dis ? "DISABLED" : "ACTIVE") << "\n\n";
        std::cout << "Subsystem Engines (Verified Runtime Assessment):\n";

        for (const auto &[name, eng] : engines) {
            std::cout << "  • " << name << " Engine:\n";
            std::cout << "      Installed:        " << (eng.installed ? "YES" : "NO") << "\n";
            std::cout << "      Enabled:          " << (eng.enabled ? "YES" : "NO") << "\n";
            std::cout << "      Config Valid:     " << (eng.configValid ? "YES" : "NO") << "\n";
            std::cout << "      Runtime Avail:    " << (eng.runtimeAvailable ? "YES" : "NO") << "\n";
            std::cout << "      Process Loaded:   " << (eng.processLoaded ? "YES" : "NO") << "\n";
            std::cout << "      Hook Registered:  " << (eng.hookRegistered ? "YES" : "NO") << "\n";
            std::cout << "      Hook Status:      " << eng.hookInitialized << "\n";
            std::cout << "      Runtime Observed: " << (eng.runtimeObserved ? "YES" : "NO") << "\n";
            std::cout << "      Device Verified:  " << (eng.deviceVerified ? "YES" : "NO") << "\n";
            std::cout << "      Stored Gen:       " << eng.storedGeneration << "\n";
            std::cout << "      Requested Gen:    " << eng.requestedGeneration << "\n";
            std::cout << "      Applied Gen:      " << eng.appliedGeneration << "\n";
            std::cout << "      Pending Reload:   " << (eng.pendingReload ? "YES" : "NO") << "\n";
            std::cout << "      Last Event:       " << eng.lastRuntimeEvent << "\n";
            std::cout << "      Status:           " << eng.status << "\n";
            if (!eng.lastError.empty()) {
                std::cout << "      Last Error:       " << eng.lastError << "\n";
            }
            std::cout << "      Details:          " << eng.details << "\n";
        }

        std::cout << "\nStorage Directory: " << data_dir_ << "\n";
        std::cout << "===============================================\n";
        return 0;
    }

    int cmdVerify(const std::string &engine, bool clear_mode) {
        std::vector<std::string> target_engines;
        if (engine == "all" || engine.empty()) {
            target_engines = {"hma", "pif", "copg", "tee"};
        } else if (engine == "hma" || engine == "pif" || engine == "copg" || engine == "tee") {
            target_engines = {engine};
        } else {
            std::cerr << "Unknown engine for verification: " << engine << " (valid: hma, pif, copg, tee, all)\n";
            return 1;
        }

        if (clear_mode) {
            for (const auto &eng : target_engines) {
                setVerified(eng, false);
                std::cout << "External verification cleared for engine '" << eng << "'.\n";
            }
            updateStatusJson();
            return 0;
        }

        for (const auto &eng : target_engines) {
            std::cout << "[NOT_VERIFIED] " << eng << ": no external device probe provider configured. Local readiness is not device proof.\n";
        }
        return 1;
    }

    int cmdVersion(bool json_mode) {
        std::string ver = getVersion();
        if (json_mode) {
            nlohmann::json result = {
                {"moduleId", "kaorioshelper"}, {"version", ver},
                {"protocolVersion", 1}, {"statusSchemaVersion", 1},
                {"capabilities", {
                    {"configSet", true}, {"configValidate", true},
                    {"configRollback", true}, {"settingsGlobalSync", false},
                    {"atomicMultiFileSnapshot", false}, {"redactedConfigRead", false},
                    {"structuredMutationErrors", false}, {"deviceVerification", false},
                    {"screenCapture", false}
                }}
            };
            std::cout << result.dump() << "\n";
        } else {
            std::cout << "KaoriosHelper " << ver << "\n";
        }
        return 0;
    }

    int cmdEnable(const std::string &engine) {
        std::error_code ec;
        fs::create_directories(data_dir_, ec);
        if (engine == "tee" || engine == "copg") {
            std::ofstream(data_dir_ + "/" + engine + ".enabled").close();
            std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();
        } else if (engine == "hma" || engine == "pif") {
            fs::remove(data_dir_ + "/" + engine + ".disabled", ec);
            std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();
        } else {
            std::cerr << "Unknown engine: " << engine << " (valid: hma, pif, tee, copg)\n";
            return 1;
        }
        updateStatusJson();
        std::cout << "Engine '" << engine << "' enabled (reload pending).\n";
        return 0;
    }

    int cmdDisable(const std::string &engine) {
        std::error_code ec;
        fs::create_directories(data_dir_, ec);
        if (engine == "tee" || engine == "copg") {
            fs::remove(data_dir_ + "/" + engine + ".enabled", ec);
            std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();
        } else if (engine == "hma" || engine == "pif") {
            std::ofstream(data_dir_ + "/" + engine + ".disabled").close();
            std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();
        } else {
            std::cerr << "Unknown engine: " << engine << " (valid: hma, pif, tee, copg)\n";
            return 1;
        }
        updateStatusJson();
        std::cout << "Engine '" << engine << "' disabled (reload pending).\n";
        return 0;
    }

    int cmdReload(const std::string &engine) {
        std::vector<std::string> target_engines;
        if (engine == "all" || engine.empty()) {
            target_engines = {"hma", "pif", "tee", "copg"};
        } else if (engine == "hma" || engine == "pif" || engine == "tee" || engine == "copg") {
            target_engines = {engine};
        } else {
            std::cerr << "Unknown engine: " << engine << " (valid: hma, pif, tee, copg, all)\n";
            return 1;
        }

        try {
            kaorios::NativeStore store(data_dir_);
            for (const auto &eng : target_engines) {
                uint64_t gen = getStoredGeneration(eng);
                store.write(eng + ".requested_gen", std::to_string(gen) + "\n");
                store.write(eng + ".reload_pending", "");
                store.remove(eng + ".verified");
                std::cout << "PENDING: Engine '" << eng << "' reload requested (generation " << gen << ").\n";
            }
            return 0;
        } catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 1; }
    }

    static bool validateJsonSyntax(const std::string &content) {
        return kaorios::validateNativeJson("", content);
    }

    static bool validateXmlSyntax(const std::string &content) {
        if (content.empty()) return false;

        // Structural check: Root tag
        bool has_root = (content.find("<KeyboxContent") != std::string::npos && content.find("</KeyboxContent>") != std::string::npos) ||
                         (content.find("<Keybox") != std::string::npos && content.find("</Keybox>") != std::string::npos);
        if (!has_root) return false;

        // Structural check: Key element with algorithm
        size_t key_open = content.find("<Key");
        size_t key_close = content.find("</Key>");
        if (key_open == std::string::npos || key_close == std::string::npos || key_close <= key_open) {
            return false;
        }

        // Structural check: PrivateKey element
        size_t priv_open = content.find("<PrivateKey", key_open);
        size_t priv_close = content.find("</PrivateKey>", key_open);
        if (priv_open == std::string::npos || priv_close == std::string::npos || priv_close <= priv_open || priv_close > key_close) {
            return false;
        }

        // Structural check: Certificate element
        size_t cert_open = content.find("<Certificate", key_open);
        size_t cert_close = content.find("</Certificate>", key_open);
        if (cert_open == std::string::npos || cert_close == std::string::npos || cert_close <= cert_open || cert_close > key_close) {
            return false;
        }

        return true;
    }

    static std::string configName(const std::string &engine) {
        if (engine == "pif" || engine == "hma" || engine == "kaorios") return engine + ".json";
        if (engine == "copg") return "spoof.json";
        if (engine == "tee") return "keybox.xml";
        throw std::runtime_error("Invalid config engine");
    }

    static bool validConfig(const std::string &engine, const std::string &content) {
        return engine == "tee" ? validateXmlSyntax(content) : kaorios::validateNativeJson(engine, content);
    }

    static std::string readConfigSource(const std::string &path) {
        int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat st{};
        if (fd < 0) throw std::runtime_error("Source open failed");
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022) || st.st_size > 16*1024*1024) {
            close(fd); throw std::runtime_error("Untrusted source file");
        }
        std::string content;
        char buf[8192]; ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) != 0) {
            if (n < 0) { if (errno == EINTR) continue; close(fd); throw std::runtime_error("Source read failed"); }
            content.append(buf, n);
            if (content.size() > 16*1024*1024) { close(fd); throw std::runtime_error("Source too large"); }
        }
        close(fd);
        return content;
    }

    int publishConfig(const std::string &engine, const std::string &src_path, bool rollback) {
        try {
            const auto filename = configName(engine);
            kaorios::NativeStore store(data_dir_);
            std::string content;
            if (rollback) content = store.read(filename + ".lkg");
            else content = readConfigSource(src_path);
            if (!validConfig(engine, content)) throw std::runtime_error("Configuration validation failed");
            const auto previous = store.read(filename, true);
            // Set retains full replacement semantics, including deliberate field removal.
            std::string generation = store.read(engine + ".stored_gen", true);
            uint64_t gen = previous.empty() ? 0 : 1;
            if (!generation.empty()) {
                size_t end = 0;
                if (generation[0] < '0' || generation[0] > '9') throw std::runtime_error("Invalid stored generation");
                gen = std::stoull(generation, &end);
                if (generation.find_first_not_of(" \r\n\t", end) != std::string::npos) throw std::runtime_error("Invalid stored generation");
            }
            if (gen == UINT64_MAX) throw std::runtime_error("Generation exhausted");
            ++gen;
            for (const auto &name : {filename, filename + ".lkg", engine + ".stored_gen", engine + ".requested_gen", engine + ".verified", engine + ".reload_pending"}) store.check(name);
            if (!rollback && validConfig(engine, previous)) store.write(filename + ".lkg", previous);
            // Metadata is published first: interrupted writes fail closed as pending,
            // never leaving the previous ACK current for newly published content.
            store.remove(engine + ".verified");
            store.write(engine + ".reload_pending", "");
            store.write(engine + ".requested_gen", std::to_string(gen) + "\n");
            store.write(engine + ".stored_gen", std::to_string(gen) + "\n");
            store.write(filename, content);
            std::cout << "Configuration published: " << filename << " (generation " << gen << ").\n";
            return 0;
        } catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 1; }
    }

    int cmdConfigSet(const std::string &engine, const std::string &source) { return publishConfig(engine, source, false); }
    int cmdConfigRollback(const std::string &engine) { return publishConfig(engine, "", true); }

    int cmdConfigValidate(const std::string &engine, const std::string &source = "") {
        try {
            configName(engine);
            std::string content;
            if (source.empty()) { kaorios::NativeStore store(data_dir_); content = store.read(configName(engine)); }
            else content = readConfigSource(source);
            if (!validConfig(engine, content)) throw std::runtime_error("Configuration validation failed");
            std::cout << engine << ": valid\n"; return 0;
        } catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 1; }
    }

    int cmdConfigSync(const std::string &engine) {
        if (engine != "all") { try { configName(engine); } catch (...) { return 1; } }
        std::cerr << "Settings.Global sync unsupported: repository defines no native-config-to-ROM Settings.Global mapping; no settings were changed.\n";
        return 1;
    }

    int cmdConfigGet(const std::string &engine) {
        std::string filename;
        if (engine == "kaorios") filename = "kaorios.json";
        else if (engine == "pif") filename = "pif.json";
        else if (engine == "hma") filename = "hma.json";
        else if (engine == "copg") filename = "spoof.json";
        else if (engine == "tee") filename = "keybox.xml";
        else {
            std::cerr << "Invalid engine: " << engine << "\n";
            return 1;
        }

        try {
            kaorios::NativeStore store(data_dir_);
            std::cout << store.read(filename);
            return 0;
        } catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 1; }
    }

    int cmdDiagnostics() {
        std::cout << "KaoriosHelper Diagnostic Information\n";
        std::cout << "-------------------------------------\n";
        std::cout << "Module Directory: " << mod_dir_ << "\n";
        std::cout << "Data Directory:   " << data_dir_ << "\n";
        std::cout << "Version:          " << getVersion() << "\n\n";

        std::cout << "Configuration Files:\n";
        for (const auto &entry : {"pif.json", "pif.json.lkg", "hma.json", "hma.json.lkg", "spoof.json", "spoof.json.lkg", "keybox.xml", "keybox.xml.lkg", "status.json"}) {
            std::string p = data_dir_ + "/" + entry;
            struct stat st{};
            if (stat(p.c_str(), &st) == 0) {
                std::cout << "  • " << entry << " - " << st.st_size << " bytes, mode 0"
                          << std::oct << (st.st_mode & 0777) << std::dec << "\n";
            } else {
                std::cout << "  • " << entry << " - [NOT FOUND]\n";
            }
        }
        std::cout << "\nActive Marker Files:\n";
        for (const auto &m : {"hma.disabled", "pif.disabled", "tee.enabled", "copg.enabled"}) {
            std::string p = data_dir_ + "/" + m;
            struct stat st{};
            std::cout << "  • " << m << ": " << (stat(p.c_str(), &st) == 0 ? "EXISTS" : "ABSENT") << "\n";
        }
        return 0;
    }

private:
    std::string mod_dir_;
    std::string data_dir_;
};

int main(int argc, char **argv) {
    bool json_mode = false;
    std::string action = "status";
    std::vector<std::string> args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json") {
            json_mode = true;
        } else if (action == "status" && args.empty()) {
            action = arg;
        } else {
            args.push_back(arg);
        }
    }

    HelperCtl ctl;

    if (action == "status") {
        return ctl.cmdStatus(json_mode);
    } else if (action == "version") {
        return ctl.cmdVersion(json_mode);
    } else if (action == "enable") {
        if (args.empty()) {
            std::cerr << "Usage: helperctl enable <hma|pif|tee|copg>\n";
            return 1;
        }
        return ctl.cmdEnable(args[0]);
    } else if (action == "disable") {
        if (args.empty()) {
            std::cerr << "Usage: helperctl disable <hma|pif|tee|copg>\n";
            return 1;
        }
        return ctl.cmdDisable(args[0]);
    } else if (action == "reload") {
        std::string eng = args.empty() ? "all" : args[0];
        return ctl.cmdReload(eng);
    } else if (action == "verify") {
        std::string eng = "all";
        bool clear_mode = false;
        for (const auto &a : args) {
            if (a == "--clear") clear_mode = true;
            else eng = a;
        }
        return ctl.cmdVerify(eng, clear_mode);
    } else if (action == "config") {
        if (args.size() >= 2 && args[0] == "sync") {
            return ctl.cmdConfigSync(args[1]);
        } else if (args.size() >= 2 && args[0] == "validate") {
            return ctl.cmdConfigValidate(args[1], args.size() > 2 ? args[2] : "");
        } else if (args.size() >= 2 && args[0] == "status") {
            return ctl.cmdConfigValidate(args[1]);
        } else if (args.size() >= 3 && args[0] == "set") {
            return ctl.cmdConfigSet(args[1], args[2]);
        } else if (args.size() >= 2 && args[0] == "rollback") {
            return ctl.cmdConfigRollback(args[1]);
        } else if (args.size() >= 2 && args[0] == "get") {
            return ctl.cmdConfigGet(args[1]);
        } else {
            std::cerr << "Usage: helperctl config {set|get|rollback|validate|status|sync} <pif|hma|tee|copg|kaorios|all> [file]\n";
            return 1;
        }
    } else if (action == "diagnostics") {
        return ctl.cmdDiagnostics();
    } else {
        std::cerr << "Usage: helperctl {status|version|enable|disable|reload|verify|config|diagnostics} [--json]\n";
        return 1;
    }
}
