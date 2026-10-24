/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 *
 * Runtime Acknowledgement Protocol
 * Secure, versioned IPC protocol for tracking runtime hook specialization and observation.
 */

#pragma once

#include <string>
#include <cstdint>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "../third_party/json/nlohmann/json.hpp"

namespace kaorios {

enum class AckEventType {
    PROCESS_LOADED = 1,
    CONFIG_LOADED = 2,
    HOOK_REGISTERED = 3,
    HOOK_INITIALIZED = 4,
    HOOK_OBSERVED = 5,
    APPLY_FAILED = 6
};

inline const char* ackEventTypeToString(AckEventType type) {
    switch (type) {
        case AckEventType::PROCESS_LOADED: return "PROCESS_LOADED";
        case AckEventType::CONFIG_LOADED: return "CONFIG_LOADED";
        case AckEventType::HOOK_REGISTERED: return "HOOK_REGISTERED";
        case AckEventType::HOOK_INITIALIZED: return "HOOK_INITIALIZED";
        case AckEventType::HOOK_OBSERVED: return "HOOK_OBSERVED";
        case AckEventType::APPLY_FAILED: return "APPLY_FAILED";
        default: return "UNKNOWN";
    }
}

inline AckEventType stringToAckEventType(const std::string &str) {
    if (str == "PROCESS_LOADED") return AckEventType::PROCESS_LOADED;
    if (str == "CONFIG_LOADED") return AckEventType::CONFIG_LOADED;
    if (str == "HOOK_REGISTERED") return AckEventType::HOOK_REGISTERED;
    if (str == "HOOK_INITIALIZED") return AckEventType::HOOK_INITIALIZED;
    if (str == "HOOK_OBSERVED") return AckEventType::HOOK_OBSERVED;
    return AckEventType::APPLY_FAILED;
}

struct RuntimeAck {
    int schemaVersion{1};
    int protocolVersion{1};
    std::string engine;         // "hma", "pif", "copg", "tee"
    std::string processName;    // "com.google.android.gms.unstable", "system_server"
    pid_t pid{0};
    uid_t uid{0};
    AckEventType eventType{AckEventType::PROCESS_LOADED};
    uint64_t configGeneration{0};
    int initResult{0};         // 0 = SUCCESS, negative = ERROR
    int errorCode{0};
    uint64_t timestamp{0};
    std::string detail;

    nlohmann::json toJson() const {
        return nlohmann::json{
            {"schemaVersion", schemaVersion},
            {"protocolVersion", protocolVersion},
            {"engine", engine},
            {"processName", processName},
            {"pid", pid},
            {"uid", uid},
            {"eventType", ackEventTypeToString(eventType)},
            {"configGeneration", configGeneration},
            {"initResult", initResult},
            {"errorCode", errorCode},
            {"timestamp", timestamp},
            {"detail", detail}
        };
    }

    static bool fromJson(const nlohmann::json &j, RuntimeAck &out) {
        try {
            if (!j.is_object()) return false;
            if (!j.contains("schemaVersion") || !j.contains("engine") || !j.contains("eventType")) {
                return false;
            }
            out.schemaVersion = j.value("schemaVersion", 1);
            out.protocolVersion = j.value("protocolVersion", 1);
            out.engine = j.value("engine", "");
            out.processName = j.value("processName", "");
            out.pid = j.value("pid", 0);
            out.uid = j.value("uid", 0);
            out.eventType = stringToAckEventType(j.value("eventType", "APPLY_FAILED"));
            out.configGeneration = j.value("configGeneration", 0ULL);
            out.initResult = j.value("initResult", 0);
            out.errorCode = j.value("errorCode", 0);
            out.timestamp = j.value("timestamp", 0ULL);
            out.detail = j.value("detail", "");
            return true;
        } catch (...) {
            return false;
        }
    }
};

inline bool validateRuntimeAck(const RuntimeAck &ack, std::string &reason) {
    if (ack.schemaVersion != 1) {
        reason = "Unsupported schemaVersion";
        return false;
    }
    if (ack.protocolVersion != 1) {
        reason = "Unsupported protocolVersion";
        return false;
    }
    if (ack.engine != "hma" && ack.engine != "pif" && ack.engine != "copg" && ack.engine != "tee") {
        reason = "Invalid engine identifier";
        return false;
    }
    if (ack.processName.empty()) {
        reason = "Missing processName";
        return false;
    }
    if (ack.pid <= 0) {
        reason = "Invalid PID";
        return false;
    }
    return true;
}

inline bool writeRuntimeAckDirect(const std::string &runtime_dir, const RuntimeAck &ack) {
    std::string err;
    if (!validateRuntimeAck(ack, err)) {
        return false;
    }

    mkdir(runtime_dir.c_str(), 0755);

    std::string target_file = runtime_dir + "/" + ack.engine + ".ack.json";
    std::string compat_file = runtime_dir + "/" + ack.engine + ".json";
    std::string tmp_file = target_file + ".tmp." + std::to_string(getpid());

    std::string serialized = ack.toJson().dump(2);

    int fd = open(tmp_file.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return false;

    ssize_t written = write(fd, serialized.data(), serialized.size());
    close(fd);

    if (written != static_cast<ssize_t>(serialized.size())) {
        unlink(tmp_file.c_str());
        return false;
    }

    if (rename(tmp_file.c_str(), target_file.c_str()) != 0) {
        unlink(tmp_file.c_str());
        return false;
    }

    // Also mirror to compat file
    unlink(compat_file.c_str());
    link(target_file.c_str(), compat_file.c_str());

    return true;
}

inline bool readRuntimeAck(const std::string &runtime_dir, const std::string &engine, RuntimeAck &out) {
    std::string target_file = runtime_dir + "/" + engine + ".ack.json";
    if (access(target_file.c_str(), R_OK) != 0) {
        target_file = runtime_dir + "/" + engine + ".json";
    }
    int fd = open(target_file.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 65536) {
        close(fd);
        return false;
    }

    std::string buf(st.st_size, '\0');
    ssize_t n = read(fd, &buf[0], st.st_size);
    close(fd);

    if (n != st.st_size) return false;

    auto j = nlohmann::json::parse(buf, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    if (!RuntimeAck::fromJson(j, out)) return false;

    std::string err;
    if (!validateRuntimeAck(out, err)) return false;

    return true;
}

} // namespace kaorios
