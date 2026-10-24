/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * KaoriosHelper Native Control CLI (helperctl)
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <cstring>
#include <cerrno>
#include "../../third_party/json/nlohmann/json.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

namespace fs = std::filesystem;

class HelperCtl {
public:
    HelperCtl(std::string mod_dir = "") {
        if (!mod_dir.empty()) {
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
            if (mod_dir_.empty() || !fs::exists(mod_dir_ + "/module.prop")) {
                mod_dir_ = "/data/adb/modules/kaorioshelper";
            }
        }
        data_dir_ = mod_dir_ + "/data";
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

    struct EngineStatus {
        std::string name;
        bool installed{true};
        bool enabled{false};
        bool configValid{false};
        bool runtimeAvailable{false};
        std::string hookInitialized{"NOT_INITIALIZED"};
        uint64_t appliedGeneration{0};
        bool pendingReload{false};
        bool deviceVerified{false};
        std::string lastError{""};
        std::string status{"DISABLED"};
        std::string details{""};
    };

    uint64_t getAppliedGeneration() {
        std::string gen_file = data_dir_ + "/generation.txt";
        std::ifstream f(gen_file);
        if (f.is_open()) {
            uint64_t gen = 0;
            if (f >> gen) return gen;
        }
        return 0;
    }

    std::map<std::string, EngineStatus> getEngineStatuses() {
        std::map<std::string, EngineStatus> res;
        struct stat st{};
        uint64_t current_gen = getAppliedGeneration();

        // ── HMA ─────────────────────────────────────────────────────────────
        EngineStatus hma;
        hma.name = "hma";
        hma.installed = true;
        hma.enabled = (stat((data_dir_ + "/hma.disabled").c_str(), &st) != 0);
        hma.pendingReload = (stat((data_dir_ + "/hma.reload_pending").c_str(), &st) == 0);
        hma.appliedGeneration = current_gen;
        hma.deviceVerified = false; // Pending physical device testing

        // Check classes.dex
        bool dex_exists = (stat((data_dir_ + "/classes.dex").c_str(), &st) == 0 && st.st_size > 0) ||
                          (stat((mod_dir_ + "/classes.dex").c_str(), &st) == 0 && st.st_size > 0);
        hma.runtimeAvailable = dex_exists;

        // Check hma.json
        if (stat((data_dir_ + "/hma.json").c_str(), &st) == 0 && st.st_size > 0) {
            std::ifstream f(data_dir_ + "/hma.json");
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            auto j = nlohmann::json::parse(content, nullptr, false);
            hma.configValid = (!j.is_discarded() && j.is_object());
            if (!hma.configValid) hma.lastError = "hma.json malformed JSON syntax";
        } else {
            hma.configValid = false;
            hma.lastError = "hma.json configuration missing or empty";
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
            // HMA without DEX must report INCOMPLETE_RUNTIME
            hma.status = "INCOMPLETE_RUNTIME";
            hma.hookInitialized = "INCOMPLETE_RUNTIME";
            hma.lastError = "classes.dex missing: upstream HMA DEX payload not bundled";
            hma.details = "system_server in-memory DEX loader waiting for classes.dex";
        } else if (hma.pendingReload) {
            hma.status = "PENDING_RELOAD";
            hma.hookInitialized = "INITIALIZED";
            hma.details = "HMA DEX present; pending system_server restart";
        } else {
            bool ack = (stat((data_dir_ + "/runtime/hma.ack").c_str(), &st) == 0);
            if (ack) {
                hma.status = "ACTIVE";
                hma.hookInitialized = "INITIALIZED";
                hma.deviceVerified = true;
                hma.lastError = "";
                hma.details = "system_server in-memory DEX injection & package filtering verified";
            } else {
                hma.status = "NOT_VERIFIED";
                hma.hookInitialized = "NOT_VERIFIED";
                hma.lastError = "Runtime hook acknowledgement pending system_server specialization";
                hma.details = "HMA configured with DEX; awaiting system_server execution";
            }
        }
        res["hma"] = hma;

        // ── PIF ─────────────────────────────────────────────────────────────
        EngineStatus pif;
        pif.name = "pif";
        pif.installed = true;
        pif.enabled = (stat((data_dir_ + "/pif.disabled").c_str(), &st) != 0);
        pif.pendingReload = (stat((data_dir_ + "/pif.reload_pending").c_str(), &st) == 0);
        pif.appliedGeneration = current_gen;
        pif.deviceVerified = false; // Pending physical device testing
        pif.runtimeAvailable = true; // ByteDance ShadowHook property interceptor available

        if (stat((data_dir_ + "/pif.json").c_str(), &st) == 0 && st.st_size > 0) {
            std::ifstream f(data_dir_ + "/pif.json");
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            auto j = nlohmann::json::parse(content, nullptr, false);
            pif.configValid = (!j.is_discarded() && j.is_object());
            if (!pif.configValid) pif.lastError = "pif.json malformed JSON syntax";
        } else {
            pif.configValid = false;
            pif.lastError = "pif.json configuration missing or empty";
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
            pif.hookInitialized = "NOT_VERIFIED";
            pif.details = "PIF configuration updated; pending target reload";
        } else {
            bool ack = (stat((data_dir_ + "/runtime/pif.ack").c_str(), &st) == 0);
            if (ack) {
                pif.status = "ACTIVE";
                pif.hookInitialized = "INITIALIZED";
                pif.deviceVerified = true;
                pif.lastError = "";
                pif.details = "ByteDance ShadowHook native libc property hook active and verified";
            } else {
                // PIF must report NOT_VERIFIED until runtime acknowledgements exist
                pif.status = "NOT_VERIFIED";
                pif.hookInitialized = "NOT_VERIFIED";
                pif.lastError = "Runtime hook acknowledgement pending target process specialization";
                pif.details = "ByteDance ShadowHook property interceptor configured";
            }
        }
        res["pif"] = pif;

        // ── TEE ─────────────────────────────────────────────────────────────
        EngineStatus tee;
        tee.name = "tee";
        tee.installed = true;
        tee.enabled = (stat((data_dir_ + "/tee.enabled").c_str(), &st) == 0);
        tee.pendingReload = (stat((data_dir_ + "/tee.reload_pending").c_str(), &st) == 0);
        tee.appliedGeneration = current_gen;
        tee.deviceVerified = false;
        tee.runtimeAvailable = false; // Keystore2 ptrace/AIDL integration is not bundled

        if (stat((data_dir_ + "/keybox.xml").c_str(), &st) == 0 && st.st_size > 0) {
            std::ifstream f(data_dir_ + "/keybox.xml");
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            tee.configValid = validateXmlSyntax(content);
            if (!tee.configValid) tee.lastError = "keybox.xml invalid XML structure";
        } else {
            tee.configValid = false;
            tee.lastError = "keybox.xml missing or empty";
        }

        // TEE without Keystore2 ptrace/AIDL must report INCOMPLETE_RUNTIME
        if (!tee.enabled) {
            tee.status = "DISABLED";
            tee.hookInitialized = "INCOMPLETE_RUNTIME";
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
        copg.appliedGeneration = current_gen;
        copg.deviceVerified = false;
        copg.runtimeAvailable = true;

        std::string copg_file = data_dir_ + "/spoof.json";
        if (stat(copg_file.c_str(), &st) != 0 || st.st_size == 0) {
            copg_file = data_dir_ + "/COPG.json";
        }
        if (stat(copg_file.c_str(), &st) == 0 && st.st_size > 0) {
            std::ifstream f(copg_file);
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            auto j = nlohmann::json::parse(content, nullptr, false);
            copg.configValid = (!j.is_discarded() && j.is_object());
            if (!copg.configValid) copg.lastError = "spoof.json malformed JSON syntax";
        } else {
            copg.configValid = false;
            copg.lastError = "spoof.json missing or empty";
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
            copg.hookInitialized = "NOT_VERIFIED";
            copg.details = "COPG configuration updated; pending game reload";
        } else {
            bool ack = (stat((data_dir_ + "/runtime/copg.ack").c_str(), &st) == 0);
            if (ack) {
                copg.status = "ACTIVE";
                copg.hookInitialized = "INITIALIZED";
                copg.deviceVerified = true;
                copg.lastError = "";
                copg.details = "Per-app Build spoofing active and verified";
            } else {
                // COPG must report NOT_VERIFIED until runtime acknowledgements exist
                copg.status = "NOT_VERIFIED";
                copg.hookInitialized = "NOT_VERIFIED";
                copg.lastError = "Runtime hook acknowledgement pending target app specialization";
                copg.details = "Game profile and device identity virtualization configured";
            }
        }
        res["copg"] = copg;

        return res;
    }

    bool updateStatusJson() {
        fs::create_directories(data_dir_);
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
            ej["hookInitialized"] = eng.hookInitialized;
            ej["appliedGeneration"] = eng.appliedGeneration;
            ej["pendingReload"] = eng.pendingReload;
            ej["deviceVerified"] = eng.deviceVerified;
            ej["lastError"] = eng.lastError;
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
                ej["hookInitialized"] = eng.hookInitialized;
                ej["appliedGeneration"] = eng.appliedGeneration;
                ej["pendingReload"] = eng.pendingReload;
                ej["deviceVerified"] = eng.deviceVerified;
                ej["lastError"] = eng.lastError;
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
            std::cout << "      Hook Status:      " << eng.hookInitialized << "\n";
            std::cout << "      Applied Gen:      " << eng.appliedGeneration << "\n";
            std::cout << "      Pending Reload:   " << (eng.pendingReload ? "YES" : "NO") << "\n";
            std::cout << "      Device Verified:  " << (eng.deviceVerified ? "YES" : "NO") << "\n";
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

    int cmdVersion(bool json_mode) {
        std::string ver = getVersion();
        if (json_mode) {
            std::cout << "{\"moduleId\": \"kaorioshelper\", \"version\": \"" << ver << "\"}\n";
        } else {
            std::cout << "KaoriosHelper " << ver << "\n";
        }
        return 0;
    }

    int cmdEnable(const std::string &engine) {
        fs::create_directories(data_dir_);
        if (engine == "tee" || engine == "copg") {
            std::ofstream(data_dir_ + "/" + engine + ".enabled").close();
            std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();
        } else if (engine == "hma" || engine == "pif") {
            fs::remove(data_dir_ + "/" + engine + ".disabled");
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
        fs::create_directories(data_dir_);
        if (engine == "tee" || engine == "copg") {
            fs::remove(data_dir_ + "/" + engine + ".enabled");
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
        if (engine == "all" || engine.empty()) {
            for (const auto &eng : {"hma", "pif", "tee", "copg"}) {
                fs::remove(data_dir_ + "/" + eng + ".reload_pending");
            }
            std::cout << "Acknowledged and cleared reload markers for all engines.\n";
        } else {
            fs::remove(data_dir_ + "/" + engine + ".reload_pending");
            std::cout << "Acknowledged and cleared reload marker for " << engine << ".\n";
        }
        updateStatusJson();
        return 0;
    }

    static bool validateJsonSyntax(const std::string &content) {
        int brace_depth = 0;
        int bracket_depth = 0;
        bool in_string = false;
        bool escape = false;

        for (char c : content) {
            if (escape) {
                escape = false;
                continue;
            }
            if (c == '\\' && in_string) {
                escape = true;
                continue;
            }
            if (c == '"') {
                in_string = !in_string;
                continue;
            }
            if (in_string) continue;

            if (c == '{') brace_depth++;
            else if (c == '}') brace_depth--;
            else if (c == '[') bracket_depth++;
            else if (c == ']') bracket_depth--;

            if (brace_depth < 0 || bracket_depth < 0) return false;
        }
        return (!in_string && brace_depth == 0 && bracket_depth == 0 && !content.empty());
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

    int cmdConfigSet(const std::string &engine, const std::string &src_path) {
        std::string filename;
        bool is_xml = false;
        if (engine == "pif") filename = "pif.json";
        else if (engine == "hma") filename = "hma.json";
        else if (engine == "copg") filename = "spoof.json";
        else if (engine == "tee") { filename = "keybox.xml"; is_xml = true; }
        else {
            std::cerr << "Invalid engine for config: " << engine << "\n";
            return 1;
        }

        // Security check: reject symlinks
        struct stat lst{};
        if (lstat(src_path.c_str(), &lst) != 0) {
            std::cerr << "Error: Source configuration file does not exist: " << src_path << "\n";
            return 1;
        }
        if (S_ISLNK(lst.st_mode)) {
            std::cerr << "Security violation: Symlinks not permitted for configuration source.\n";
            return 1;
        }

        std::ifstream in(src_path);
        if (!in.is_open()) {
            std::cerr << "Error: Cannot open source file: " << src_path << "\n";
            return 1;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string content = buffer.str();
        in.close();

        // Validate syntax before touching destination
        if (is_xml) {
            if (!validateXmlSyntax(content)) {
                std::cerr << "Validation failed: Invalid Keybox XML format.\n";
                return 1;
            }
        } else {
            if (!validateJsonSyntax(content)) {
                std::cerr << "Validation failed: Malformed JSON syntax.\n";
                return 1;
            }
        }

        // Atomic write with flock and LKG backup
        fs::create_directories(data_dir_);
        std::string lock_path = data_dir_ + "/.config.lock";
        int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_EX);
        }

        std::string tmp_dest = data_dir_ + "/." + filename + ".tmp." + std::to_string(getpid());
        std::string final_dest = data_dir_ + "/" + filename;
        std::string lkg_dest = data_dir_ + "/" + filename + ".lkg";

        int out_fd = open(tmp_dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out_fd < 0) {
            std::cerr << "Error opening temporary staging file: " << strerror(errno) << "\n";
            if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
            return 1;
        }

        ssize_t written = write(out_fd, content.data(), content.size());
        fchmod(out_fd, 0600);
        fsync(out_fd);
        close(out_fd);

        if (written != static_cast<ssize_t>(content.size())) {
            unlink(tmp_dest.c_str());
            std::cerr << "Error: Incomplete write to staging file.\n";
            if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
            return 1;
        }

        // Save existing valid config as LKG backup before replacing
        struct stat curr_st{};
        if (stat(final_dest.c_str(), &curr_st) == 0 && curr_st.st_size > 0) {
            std::ifstream cur_in(final_dest, std::ios::binary);
            std::ofstream lkg_out(lkg_dest, std::ios::binary | std::ios::trunc);
            if (cur_in && lkg_out) {
                lkg_out << cur_in.rdbuf();
                lkg_out.flush();
                chmod(lkg_dest.c_str(), 0600);
            }
        }

        if (rename(tmp_dest.c_str(), final_dest.c_str()) != 0) {
            std::cerr << "Error replacing target file: " << strerror(errno) << "\n";
            unlink(tmp_dest.c_str());
            if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
            return 1;
        }

        // Set reload marker
        std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();

        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }

        updateStatusJson();
        std::cout << "Successfully updated " << filename << " atomically with LKG backup (0600 root:root).\n";
        return 0;
    }

    int cmdConfigRollback(const std::string &engine) {
        std::string filename;
        if (engine == "pif") filename = "pif.json";
        else if (engine == "hma") filename = "hma.json";
        else if (engine == "copg") filename = "spoof.json";
        else if (engine == "tee") filename = "keybox.xml";
        else {
            std::cerr << "Invalid engine for rollback: " << engine << "\n";
            return 1;
        }

        std::string lkg_dest = data_dir_ + "/" + filename + ".lkg";
        std::string final_dest = data_dir_ + "/" + filename;

        struct stat st{};
        if (stat(lkg_dest.c_str(), &st) != 0 || st.st_size == 0) {
            std::cerr << "No Last-Known-Good (LKG) backup found for " << engine << " (" << lkg_dest << ").\n";
            return 1;
        }

        std::ifstream lkg_in(lkg_dest, std::ios::binary);
        std::string tmp_dest = data_dir_ + "/." + filename + ".rb.tmp." + std::to_string(getpid());
        std::ofstream tmp_out(tmp_dest, std::ios::binary | std::ios::trunc);
        if (!lkg_in || !tmp_out) {
            std::cerr << "Failed to read LKG backup file.\n";
            return 1;
        }
        tmp_out << lkg_in.rdbuf();
        tmp_out.flush();
        tmp_out.close();

        chmod(tmp_dest.c_str(), 0600);
        if (rename(tmp_dest.c_str(), final_dest.c_str()) != 0) {
            std::cerr << "Failed to restore LKG config: " << strerror(errno) << "\n";
            unlink(tmp_dest.c_str());
            return 1;
        }

        std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();
        updateStatusJson();
        std::cout << "Successfully rolled back " << engine << " to Last-Known-Good (LKG) configuration.\n";
        return 0;
    }

    int cmdConfigGet(const std::string &engine) {
        std::string filename;
        if (engine == "pif") filename = "pif.json";
        else if (engine == "hma") filename = "hma.json";
        else if (engine == "copg") filename = "spoof.json";
        else if (engine == "tee") filename = "keybox.xml";
        else {
            std::cerr << "Invalid engine: " << engine << "\n";
            return 1;
        }

        std::string path = data_dir_ + "/" + filename;
        std::ifstream f(path);
        if (!f.is_open()) {
            std::cerr << "Configuration file not found: " << path << "\n";
            return 1;
        }
        std::cout << f.rdbuf();
        return 0;
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
    } else if (action == "config") {
        if (args.size() >= 3 && args[0] == "set") {
            return ctl.cmdConfigSet(args[1], args[2]);
        } else if (args.size() >= 2 && args[0] == "rollback") {
            return ctl.cmdConfigRollback(args[1]);
        } else if (args.size() >= 2 && args[0] == "get") {
            return ctl.cmdConfigGet(args[1]);
        } else {
            std::cerr << "Usage: helperctl config {set|get|rollback} <pif|hma|tee|copg> [file]\n";
            return 1;
        }
    } else if (action == "diagnostics") {
        return ctl.cmdDiagnostics();
    } else {
        std::cerr << "Usage: helperctl {status|version|enable|disable|reload|config|diagnostics} [--json]\n";
        return 1;
    }
}
