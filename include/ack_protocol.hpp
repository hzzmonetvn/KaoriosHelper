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
#include <atomic>
#include <cerrno>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
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

    // Engine ownership & process authorization
    if (ack.engine == "hma") {
        if (ack.processName != "system_server" && ack.processName != "android") {
            reason = "HMA engine ownership violation: process must be system_server";
            return false;
        }
        if (ack.uid != 1000 && ack.uid != 0) {
            reason = "HMA engine UID violation: UID must be 1000 or 0";
            return false;
        }
    } else if (ack.engine == "pif") {
        if (ack.processName != "com.google.android.gms" &&
            ack.processName != "com.google.android.gms.unstable" &&
            ack.processName != "com.android.vending") {
            reason = "PIF engine ownership violation: process must be GMS";
            return false;
        }
    } else if (ack.engine == "tee") {
        if (ack.processName != "com.google.android.gms" &&
            ack.processName != "com.google.android.gms.unstable") {
            reason = "TEE engine ownership violation: process must be GMS";
            return false;
        }
    }
    return true;
}

inline int openRuntimeAckDirectory(const std::string &path, bool create) {
    // Walk every component without following symlinks, then pin the directory.
    int fd = open(path.size() && path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        std::string part = path.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") { close(fd); return -1; }
        if (create && end == path.size() && mkdirat(fd, part.c_str(), 0755) != 0 && errno != EEXIST) {
            close(fd); return -1;
        }
        int next = openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        if (next < 0) return -1;
        fd = next;
    }
    struct stat st{};
    if (path.empty() || fstat(fd, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        close(fd); return -1;
    }
    return fd;
}

inline bool writeRuntimeAckDirect(const std::string &runtime_dir, const RuntimeAck &ack) {
    std::string err;
    if (!validateRuntimeAck(ack, err)) {
        return false;
    }

    std::string serialized = ack.toJson().dump(2);
    if (serialized.empty() || serialized.size() > 65536) return false;
    int dir = openRuntimeAckDirectory(runtime_dir, true);
    if (dir < 0) return false;
    int locked;
    do { locked = flock(dir, LOCK_EX); } while (locked != 0 && errno == EINTR);
    if (locked != 0) { close(dir); return false; }
    std::string target = ack.engine + ".ack.json";
    std::string compat = ack.engine + ".json";
    static std::atomic<uint64_t> sequence{0};
    std::string staging;
    int fd = -1;
    for (int attempt = 0; attempt < 128; ++attempt) {
        staging = target + ".tmp." + std::to_string(getpid()) + "." + std::to_string(sequence.fetch_add(1));
        fd = openat(dir, staging.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { close(dir); return false; }
    size_t offset = 0;
    while (offset < serialized.size()) {
        ssize_t n = write(fd, serialized.data() + offset, serialized.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += static_cast<size_t>(n);
    }
    bool ok = offset == serialized.size() && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    // Link this exact staged inode for compatibility, rather than racing another writer's target.
    std::string compat_staging = staging + ".compat";
    if (ok) ok = linkat(dir, staging.c_str(), dir, compat_staging.c_str(), 0) == 0;
    if (ok) ok = renameat(dir, staging.c_str(), dir, target.c_str()) == 0;
    if (ok) ok = renameat(dir, compat_staging.c_str(), dir, compat.c_str()) == 0;
    if (ok) ok = fsync(dir) == 0;
    unlinkat(dir, staging.c_str(), 0);
    unlinkat(dir, compat_staging.c_str(), 0);
    close(dir);
    return ok;
}

inline bool readRuntimeAck(const std::string &runtime_dir, const std::string &engine, RuntimeAck &out) {
    if (engine != "hma" && engine != "pif" && engine != "tee" && engine != "copg") return false;
    int dir = openRuntimeAckDirectory(runtime_dir, false);
    if (dir < 0) return false;
    int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
    int fd = openat(dir, (engine + ".ack.json").c_str(), flags);
    if (fd < 0 && errno == ENOENT) fd = openat(dir, (engine + ".json").c_str(), flags);
    close(dir);
    if (fd < 0) return false;

    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0022) || st.st_size <= 0 || st.st_size > 65536) {
        close(fd);
        return false;
    }

    std::string buf(static_cast<size_t>(st.st_size), '\0');
    size_t offset = 0;
    while (offset < buf.size()) {
        ssize_t n = read(fd, &buf[offset], buf.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += static_cast<size_t>(n);
    }
    char extra;
    ssize_t trailing;
    do { trailing = read(fd, &extra, 1); } while (trailing < 0 && errno == EINTR);
    close(fd);
    if (offset != buf.size() || trailing != 0) return false;

    auto j = nlohmann::json::parse(buf, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    if (!RuntimeAck::fromJson(j, out)) return false;

    std::string err;
    if (!validateRuntimeAck(out, err)) return false;

    return true;
}

} // namespace kaorios
