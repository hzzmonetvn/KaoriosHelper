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
#include <sys/random.h>
#include "../../core/config/settings_command.hpp"
#include "../../core/config/settings_adapter.hpp"

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
        return 0; // Legacy payloads need explicit migration, not an invented generation.
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
        kaorios::NativeStore store(data_dir_);
        for (const auto *engine : {"hma", "pif", "tee", "copg", "kaorios"}) {
            const auto filename = configName(engine);
            for (const auto &name : {filename, std::string(engine) + ".stored_gen", std::string(engine) + ".requested_gen",
                    std::string(engine) + ".request.json", std::string(engine) + ".enabled", std::string(engine) + ".disabled",
                    std::string(engine) + ".reload_pending", std::string(engine) + ".verified"}) store.check(name);
        }
        store.check("COPG.json");
        std::map<std::string, EngineStatus> res;
        struct stat st{};
        const bool dexExists = (stat((data_dir_ + "/classes.dex").c_str(), &st) == 0 && st.st_size > 0) ||
            (stat((mod_dir_ + "/classes.dex").c_str(), &st) == 0 && st.st_size > 0);
        // Configuration and metadata come from one recovered, locked view.
        // Legacy evidence remains diagnostic only; readiness is not device proof.
        for (const auto *engine : {"hma", "pif", "tee", "copg", "kaorios"}) {
            const std::string name(engine);
            auto &s = res[name];
            s.name = name;
            s.enabled = name != "kaorios" && ((name == "hma" || name == "pif")
                ? !store.exists(name + ".disabled") : store.exists(name + ".enabled"));
            s.runtimeAvailable = name == "pif" || name == "copg" || (name == "hma" && dexExists);
            s.deviceVerified = false;
            s.lastVerifiedTimestamp = 0;
            s.storedGeneration = parseGeneration(store.read(name + ".stored_gen", true));
            s.requestedGeneration = parseGeneration(store.read(name + ".requested_gen", true));
            const auto content = store.read(configName(name), true);
            s.configValid = validConfig(name, content);
            s.pendingReload = store.exists(name + ".reload_pending");
            kaorios::RuntimeAck ack;
            bool hasAck = kaorios::readRuntimeAck(data_dir_ + "/runtime", name, ack);
            const auto request = nlohmann::json::parse(store.read(name + ".request.json", true), nullptr, false);
            const auto token = request.is_object() && request.contains("requestId") && request["requestId"].is_string()
                ? request["requestId"].get<std::string>() : "";
            bool current = s.enabled && s.configValid && s.runtimeAvailable && hasAck &&
                s.storedGeneration == s.requestedGeneration &&
                validRequest(request) &&
                ack.timestamp >= request["notBeforeMillis"].get<uint64_t>() &&
                kaorios::runtimeAckMatchesCurrent(ack, name, s.storedGeneration, content, token);
            using E = kaorios::AckEventType;
            bool initialized = current && ack.initResult == 0 && ack.errorCode == 0 &&
                (ack.eventType == E::HOOK_INITIALIZED || ack.eventType == E::HOOK_OBSERVED);
            s.processLoaded = current && ack.eventType != E::APPLY_FAILED && ack.initResult == 0 && ack.errorCode == 0;
            s.hookRegistered = s.processLoaded && (ack.eventType == E::HOOK_REGISTERED || initialized);
            s.runtimeObserved = initialized && ack.eventType == E::HOOK_OBSERVED;
            s.appliedGeneration = initialized ? ack.configGeneration : 0;
            s.lastRuntimeEvent = hasAck ? kaorios::ackEventTypeToString(ack.eventType) : "NONE";
            s.lastRuntimeTimestamp = hasAck ? ack.timestamp : 0;
            if (name == "kaorios") {
                s.status = s.configValid ? "INCOMPLETE_RUNTIME" : "NOT_CONFIGURED";
                s.hookInitialized = "INCOMPLETE_RUNTIME";
                s.details = "Configuration storage available; Kaorios runtime backend not implemented";
                continue;
            }
            if (!s.enabled) {
                s.status = "DISABLED";
                s.hookInitialized = "DISABLED";
                s.details = "Desired enablement is off; existing processes may still require restart";
                continue;
            }
            if (!s.configValid) {
                s.status = "NOT_CONFIGURED";
                s.hookInitialized = "NOT_INITIALIZED";
                s.details = "Canonical configuration missing or invalid; legacy payloads require explicit migration";
                continue;
            }
            if (!s.runtimeAvailable) {
                s.status = "INCOMPLETE_RUNTIME";
                s.hookInitialized = "INCOMPLETE_RUNTIME";
                s.details = name == "hma" ? "HMA classes.dex payload missing" : "TEE runtime backend not implemented";
                continue;
            }
            s.hookInitialized = initialized ? "INITIALIZED" : "NOT_INITIALIZED";
            s.pendingReload = !initialized;
            if (current && (ack.eventType == E::APPLY_FAILED || ack.initResult != 0 || ack.errorCode != 0)) {
                s.status = "FAILED"; s.hookInitialized = "FAILED";
                s.lastError = "Runtime reported configuration application failure";
                s.details = "Current process reported failure; retry or restart remains pending";
            } else if (initialized) {
                s.status = s.runtimeObserved ? "DEVICE_UNVERIFIED" : "READY";
                s.lastError.clear();
                s.details = s.runtimeObserved ? "Current process observed; physical-device verification pending"
                    : "Current process initialized; runtime observation pending";
            } else {
                s.status = "PENDING_RELOAD";
                s.lastError.clear();
                s.details = s.storedGeneration == 0 ? "Legacy configuration requires explicit migration"
                    : "Awaiting current boot/process/configuration/request acknowledgement";
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
                    {"configRollback", true}, {"settingsGlobalSync", true},
                    {"configMigration", true}, {"recoverableConfigTransactions", true},
                    {"runtimeAckV2", true},
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

    int cmdEnable(const std::string &engine) { return cmdToggle(engine, true); }
    int cmdDisable(const std::string &engine) { return cmdToggle(engine, false); }

    int cmdToggle(const std::string &engine, bool enabled) {
        if (engine != "hma" && engine != "pif" && engine != "tee" && engine != "copg") {
            std::cerr << "Unknown runtime engine\n";
            return 1;
        }
        try {
            kaorios::NativeStore store(data_dir_);
            kaorios::NativeStore::Changes changes;
            const bool defaultOn = engine == "hma" || engine == "pif";
            const auto marker = engine + (defaultOn ? ".disabled" : ".enabled");
            changes[marker] = enabled != defaultOn ? std::optional<std::string>("") : std::nullopt;
            stageRequest(changes, engine, parseGeneration(store.read(engine + ".stored_gen", true)));
            store.transaction(changes);
            std::cout << "PENDING: Desired enablement recorded; restart may be required.\n";
            return 0;
        } catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 1; }
    }

    int cmdPrepareBoot() {
        try {
            kaorios::NativeStore store(data_dir_);
            kaorios::NativeStore::Changes changes;
            for (const auto *engine : {"hma", "pif", "tee", "copg"}) {
                const std::string name(engine);
                const auto generation = parseGeneration(store.read(name + ".stored_gen", true));
                if (generation == 0) continue; // Legacy payloads still need explicit migration.
                if (!validConfig(name, store.read(configName(name), true)))
                    throw std::runtime_error("Boot configuration validation failed");
                const auto request = nlohmann::json::parse(store.read(name + ".request.json", true), nullptr, false);
                if (parseGeneration(store.read(name + ".requested_gen", true)) == generation && validRequest(request)) continue;
                stageRequest(changes, name, generation);
            }
            store.transaction(changes);
            std::cout << "Boot requests prepared; runtime acknowledgement pending\n";
            return 0;
        } catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 1; }
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
            kaorios::NativeStore::Changes changes;
            for (const auto &eng : target_engines) {
                const auto gen = parseGeneration(store.read(eng + ".stored_gen", true));
                stageRequest(changes, eng, gen);
            }
            store.transaction(changes);
            std::cout << "PENDING: Reload request recorded; existing processes may require restart.\n";
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

    static uint64_t parseGeneration(const std::string &text) {
        if (text.empty()) return 0;
        if (text[0] < '0' || text[0] > '9') throw std::runtime_error("Invalid configuration generation");
        size_t end = 0;
        const auto generation = std::stoull(text, &end);
        if (text.find_first_not_of(" \r\n\t", end) != std::string::npos) throw std::runtime_error("Invalid configuration generation");
        return generation;
    }

    static bool validRequest(const nlohmann::json &request) {
        if (!request.is_object() || !request.contains("requestId") || !request["requestId"].is_string() ||
            !request.contains("bootId") || !request["bootId"].is_string() ||
            !request.contains("notBeforeMillis") || !request["notBeforeMillis"].is_number_unsigned()) return false;
        const auto token = request["requestId"].get<std::string>();
        if (token.size() != 32 || token.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
        const auto notBefore = request["notBeforeMillis"].get<uint64_t>();
        return request["bootId"].get<std::string>() == kaorios::runtimeAckBootId() &&
            notBefore > 0 && notBefore <= kaorios::runtimeAckNowMillis();
    }

    static void stageRequest(kaorios::NativeStore::Changes &changes, const std::string &engine, uint64_t generation) {
        unsigned char entropy[16];
        size_t offset = 0;
        while (offset < sizeof(entropy)) {
            ssize_t n = getrandom(entropy + offset, sizeof(entropy) - offset, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("Runtime request identity unavailable");
            offset += static_cast<size_t>(n);
        }
        static constexpr char hex[] = "0123456789abcdef";
        std::string token;
        for (unsigned char byte : entropy) { token += hex[byte >> 4]; token += hex[byte & 15]; }
        const auto boot = kaorios::runtimeAckBootId();
        const auto now = kaorios::runtimeAckNowMillis();
        if (boot.empty() || now == 0) throw std::runtime_error("Runtime boot identity unavailable");
        changes[engine + ".request.json"] = nlohmann::json{{"requestId", token}, {"bootId", boot}, {"notBeforeMillis", now}}.dump();
        changes[engine + ".requested_gen"] = std::to_string(generation) + "\n";
        changes[engine + ".reload_pending"] = "";
        changes[engine + ".verified"] = std::nullopt;
    }

    static uint64_t stageConfig(kaorios::NativeStore &store, kaorios::NativeStore::Changes &changes,
            const std::string &engine, const std::string &content, bool rollback = false) {
        const auto filename = configName(engine);
        for (const auto &leaf : {filename, filename + ".lkg", engine + ".stored_gen", engine + ".requested_gen",
                engine + ".request.json", engine + ".reload_pending", engine + ".verified"}) store.check(leaf);
        if (!validConfig(engine, content)) throw std::runtime_error("Configuration validation failed");
        const auto previous = store.read(filename, true);
        auto gen = parseGeneration(store.read(engine + ".stored_gen", true));
        if (gen == UINT64_MAX) throw std::runtime_error("Generation exhausted");
        ++gen;
        if (!rollback && validConfig(engine, previous)) changes[filename + ".lkg"] = previous;
        changes[filename] = content;
        changes[engine + ".stored_gen"] = std::to_string(gen) + "\n";
        stageRequest(changes, engine, gen);
        return gen;
    }

    int publishConfig(const std::string &engine, const std::string &src_path, bool rollback) {
        try {
            const auto filename = configName(engine);
            kaorios::NativeStore store(data_dir_);
            std::string content;
            if (rollback) content = store.read(filename + ".lkg");
            else content = readConfigSource(src_path);
            kaorios::NativeStore::Changes changes;
            // Set retains full replacement semantics, including deliberate field removal.
            const auto gen = stageConfig(store, changes, engine, content, rollback);
            store.transaction(changes);
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
        try {
            const auto keys = kaorios::settingsKeys(engine);
            if (keys.empty()) throw std::runtime_error("Invalid synchronization engine");
            const auto settings = kaorios::captureGlobalSettings(keys);
            kaorios::NativeStore store(data_dir_);
            kaorios::NativeStore::Changes changes;
            const std::vector<std::string> engines = engine == "all"
                ? std::vector<std::string>{"pif", "tee", "copg", "hma", "kaorios"}
                : std::vector<std::string>{engine};
            std::vector<std::string> changed;
            for (const auto &name : engines) {
                const auto existing = store.read(configName(name), true);
                const auto adapted = kaorios::adaptSettings(name, existing, settings);
                if (!adapted) continue;
                stageConfig(store, changes, name, *adapted);
                changed.push_back(name);
            }
            store.transaction(changes);
            for (const auto &name : changed) std::cout << name << ": stored; runtime application pending\n";
            if (changed.empty()) std::cout << "No configuration changes\n";
            return 0;
        } catch (const std::exception &e) {
            std::cerr << "Synchronization failed: " << e.what() << "; publication may require journal recovery\n";
            return 1;
        }
    }

    int cmdConfigMigrate(const std::string &engine) {
        try {
            const std::vector<std::string> engines = engine == "all"
                ? std::vector<std::string>{"pif", "tee", "copg", "hma", "kaorios"}
                : std::vector<std::string>{engine};
            kaorios::NativeStore store(data_dir_);
            kaorios::NativeStore::Changes changes;
            for (const auto &name : engines) {
                const auto filename = configName(name);
                const auto generation = parseGeneration(store.read(name + ".stored_gen", true));
                auto content = store.read(filename, true);
                if (content.empty() && name == "copg") content = store.read("COPG.json", true);
                if (content.empty()) continue;
                if (!validConfig(name, content)) throw std::runtime_error("Legacy configuration validation failed");
                if (generation == 0) stageConfig(store, changes, name, content);
            }
            store.transaction(changes);
            std::cout << "Migration recorded; existing schemas and payload fields preserved\n";
            return 0;
        } catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 1; }
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

    try {
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
    } else if (action == "prepare-boot" && args.empty()) {
        return ctl.cmdPrepareBoot();
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
        if (args.size() == 2 && args[0] == "sync") {
            return ctl.cmdConfigSync(args[1]);
        } else if (args.size() == 2 && args[0] == "migrate") {
            return ctl.cmdConfigMigrate(args[1]);
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
            std::cerr << "Usage: helperctl config {set|get|rollback|validate|status|sync|migrate} <pif|hma|tee|copg|kaorios|all> [file]\n";
            return 1;
        }
    } else if (action == "diagnostics") {
        return ctl.cmdDiagnostics();
    } else {
        std::cerr << "Usage: helperctl {status|version|enable|disable|reload|prepare-boot|verify|config|diagnostics} [--json]\n";
        return 1;
    }
    } catch (const std::exception &) {
        std::cerr << "Control operation failed; inspect configuration integrity and retry explicitly\n";
        return 1;
    }
}
