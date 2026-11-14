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
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <set>
#include <vector>
#include <time.h>
#include "config_identity.hpp"
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
    std::string bootId;
    uint64_t processStartTicks{0};
    std::string configHash;
    std::string requestId;

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
            {"detail", detail},
            {"bootId", bootId},
            {"processStartTicks", processStartTicks},
            {"configHash", configHash},
            {"requestId", requestId}
        };
    }

    static bool fromJson(const nlohmann::json &j, RuntimeAck &out) {
        try {
            if (!j.is_object()) return false;
            const auto integer = [&](const char *key, int64_t minimum, uint64_t maximum) {
                auto field = j.find(key);
                if (field == j.end() || !field->is_number_integer()) return false;
                if (field->is_number_unsigned()) {
                    auto value = field->get<uint64_t>();
                    return value <= maximum && (minimum <= 0 || value >= static_cast<uint64_t>(minimum));
                }
                const auto value = field->get<int64_t>();
                return value >= minimum && (value < 0 || static_cast<uint64_t>(value) <= maximum);
            };
            for (const char *key : {"engine", "processName", "eventType", "detail"})
                if (!j.contains(key) || !j[key].is_string()) return false;
            if (!integer("schemaVersion", 1, 2) || !integer("protocolVersion", 1, 2) ||
                !integer("pid", 1, std::numeric_limits<pid_t>::max()) ||
                !integer("uid", 0, std::numeric_limits<uid_t>::max()) ||
                !integer("configGeneration", 0, std::numeric_limits<uint64_t>::max()) ||
                !integer("initResult", std::numeric_limits<int>::min(), std::numeric_limits<int>::max()) ||
                !integer("errorCode", std::numeric_limits<int>::min(), std::numeric_limits<int>::max()) ||
                !integer("timestamp", 0, std::numeric_limits<uint64_t>::max())) return false;
            RuntimeAck parsed;
            parsed.schemaVersion = j["schemaVersion"].get<int>();
            parsed.protocolVersion = j["protocolVersion"].get<int>();
            parsed.engine = j["engine"].get<std::string>();
            parsed.processName = j["processName"].get<std::string>();
            const auto event = j["eventType"].get<std::string>();
            parsed.eventType = stringToAckEventType(event);
            if (event != ackEventTypeToString(parsed.eventType)) return false;
            parsed.pid = j["pid"].get<pid_t>();
            parsed.uid = j["uid"].get<uid_t>();
            parsed.configGeneration = j["configGeneration"].get<uint64_t>();
            parsed.initResult = j["initResult"].get<int>();
            parsed.errorCode = j["errorCode"].get<int>();
            parsed.timestamp = j["timestamp"].get<uint64_t>();
            parsed.detail = j["detail"].get<std::string>();
            if (parsed.schemaVersion == 2) {
                for (const char *key : {"bootId", "configHash", "requestId"})
                    if (!j.contains(key) || !j[key].is_string()) return false;
                if (!integer("processStartTicks", 1, std::numeric_limits<uint64_t>::max())) return false;
                parsed.bootId = j["bootId"].get<std::string>();
                parsed.configHash = j["configHash"].get<std::string>();
                parsed.requestId = j["requestId"].get<std::string>();
                parsed.processStartTicks = j["processStartTicks"].get<uint64_t>();
            }
            out = std::move(parsed);
            return true;
        } catch (...) {
            return false;
        }
    }
};

inline bool parseRuntimeAck(const std::string &bytes, RuntimeAck &out) {
    if (bytes.empty() || bytes.size() > 65536) return false;
    using Json = nlohmann::json;
    std::vector<std::set<std::string>> keys;
    bool valid = true;
    auto callback = [&](int depth, Json::parse_event_t event, Json &value) {
        if (depth > 16) throw std::runtime_error("ACK nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) valid = false;
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    try {
        auto json = Json::parse(bytes, callback, false);
        return valid && !json.is_discarded() && RuntimeAck::fromJson(json, out);
    } catch (...) {
        return false;
    }
}

inline bool validateRuntimeAck(const RuntimeAck &ack, std::string &reason) {
    if (ack.schemaVersion != 1 && ack.schemaVersion != 2) {
        reason = "Unsupported schemaVersion";
        return false;
    }
    if (ack.protocolVersion != ack.schemaVersion) {
        reason = "Unsupported protocolVersion";
        return false;
    }
    if (ack.engine != "hma" && ack.engine != "pif" && ack.engine != "copg" && ack.engine != "tee") {
        reason = "Invalid engine identifier";
        return false;
    }
    if (ack.processName.empty() || ack.processName.size() > 4096 || ack.processName.find('\0') != std::string::npos) {
        reason = "Invalid processName";
        return false;
    }
    if (ack.pid <= 0) {
        reason = "Invalid PID";
        return false;
    }

    if (ack.schemaVersion == 2) {
        const auto hex = [](const std::string &text, size_t size) {
            if (text.size() != size) return false;
            for (char c : text) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            return true;
        };
        bool validBoot = ack.bootId.size() == 36;
        for (size_t i = 0; validBoot && i < ack.bootId.size(); ++i) {
            char c = ack.bootId[i];
            validBoot = (i == 8 || i == 13 || i == 18 || i == 23) ? c == '-' :
                ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
        if (!validBoot || !hex(ack.configHash, 64) || !hex(ack.requestId, 32) ||
            ack.processStartTicks == 0 || ack.timestamp == 0 || ack.configGeneration == 0) {
            reason = "Incomplete runtime snapshot identity";
            return false;
        }
    }
    if (ack.eventType < AckEventType::PROCESS_LOADED || ack.eventType > AckEventType::APPLY_FAILED) {
        reason = "Invalid eventType";
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

inline bool runtimeAckReadSmallFile(const std::string &path, std::string &bytes) {
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    bytes.clear();
    char buffer[4096];
    ssize_t count;
    bool ok = true;
    while ((count = read(fd, buffer, sizeof(buffer))) != 0) {
        if (count < 0) { if (errno == EINTR) continue; ok = false; break; }
        bytes.append(buffer, static_cast<size_t>(count));
        if (bytes.size() > 65536) { ok = false; break; }
    }
    close(fd);
    return ok;
}

inline std::string runtimeAckBootId() {
    std::string boot;
    if (!runtimeAckReadSmallFile("/proc/sys/kernel/random/boot_id", boot)) return "";
    if (!boot.empty() && boot.back() == '\n') boot.pop_back();
    return boot;
}

inline uint64_t runtimeAckNowMillis() {
    struct timespec now{};
    if (clock_gettime(CLOCK_BOOTTIME, &now) != 0 || now.tv_sec < 0) return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

inline bool runtimeAckReadProcessIdentity(pid_t pid, uid_t &uid, uint64_t &startTicks,
                                         std::string *processName = nullptr) {
    if (pid <= 0) return false;
    const std::string base = "/proc/" + std::to_string(pid);
    // Read the start field twice to rule out PID reuse while reading credentials/name.
    const auto start = [&](uint64_t &value) {
        std::string stat;
        if (!runtimeAckReadSmallFile(base + "/stat", stat)) return false;
        const auto end = stat.rfind(')');
        if (end == std::string::npos || end + 2 >= stat.size()) return false;
        std::istringstream fields(stat.substr(end + 2));
        std::string token;
        for (int field = 3; field <= 22; ++field) {
            if (!(fields >> token)) return false;
            if (field == 3 && (token == "Z" || token == "X" || token == "x")) return false;
        }
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() && value > 0;
    };
    uint64_t before = 0, after = 0;
    if (!start(before)) return false;
    std::string status;
    if (!runtimeAckReadSmallFile(base + "/status", status)) return false;
    const auto position = status.find("\nUid:");
    if (position == std::string::npos) return false;
    std::istringstream credentials(status.substr(position + 5));
    uint64_t real = 0, effective = 0;
    if (!(credentials >> real >> effective) || effective > std::numeric_limits<uid_t>::max()) return false;
    std::string cmdline;
    if (!runtimeAckReadSmallFile(base + "/cmdline", cmdline) || cmdline.empty()) return false;
    cmdline.resize(cmdline.find('\0') == std::string::npos ? cmdline.size() : cmdline.find('\0'));
    if (cmdline.empty() || !start(after) || before != after) return false;
    uid = static_cast<uid_t>(effective);
    startTicks = before;
    if (processName) *processName = std::move(cmdline);
    return true;
}

inline bool runtimeAckHasLiveIdentity(const RuntimeAck &ack) {
    std::string reason;
    if (ack.schemaVersion != 2 || !validateRuntimeAck(ack, reason) || ack.bootId != runtimeAckBootId()) return false;
    uid_t uid = 0;
    uint64_t start = 0;
    std::string name;
    if (!runtimeAckReadProcessIdentity(ack.pid, uid, start, &name) || uid != ack.uid ||
        start != ack.processStartTicks || name != ack.processName) return false;
    const long ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0) return false;
    uint64_t rate = static_cast<uint64_t>(ticks);
    if (start / rate > std::numeric_limits<uint64_t>::max() / 1000) return false;
    uint64_t startMillis = (start / rate) * 1000 + (start % rate) * 1000 / rate;
    uint64_t now = runtimeAckNowMillis();
    return now > 0 && ack.timestamp >= startMillis && ack.timestamp <= now;
}

inline bool bindRuntimeAckToCurrentProcess(RuntimeAck &ack, uint64_t generation,
                                           const std::string &configBytes,
                                           const std::string &requestId = "") {
    RuntimeAck bound = ack;
    bound.schemaVersion = bound.protocolVersion = 2;
    bound.pid = getpid();
    if (!runtimeAckReadProcessIdentity(bound.pid, bound.uid, bound.processStartTicks, &bound.processName)) return false;
    bound.bootId = runtimeAckBootId();
    bound.timestamp = runtimeAckNowMillis();
    bound.configGeneration = generation;
    bound.configHash = configSha256(configBytes);
    bound.requestId = requestId;
    if (!runtimeAckHasLiveIdentity(bound)) return false;
    ack = std::move(bound);
    return true;
}

// Local process evidence is not proof against a compromised engine or a root writer.
inline bool runtimeAckMatchesCurrent(const RuntimeAck &ack, const std::string &engine,
                                     uint64_t generation, const std::string &configBytes,
                                     const std::string &requestId = "") {
    return generation > 0 && !requestId.empty() && ack.engine == engine &&
        ack.configGeneration == generation && ack.requestId == requestId &&
        ack.configHash == configSha256(configBytes) && runtimeAckHasLiveIdentity(ack);
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

inline bool readRuntimeAckAt(int dir, const std::string &engine, RuntimeAck &out) {
    const int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
    int fd = openat(dir, (engine + ".ack.json").c_str(), flags);
    if (fd < 0 && errno == ENOENT) fd = openat(dir, (engine + ".json").c_str(), flags);
    if (fd < 0) return false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0022) || st.st_size <= 0 || st.st_size > 65536) {
        close(fd); return false;
    }
    std::string bytes(static_cast<size_t>(st.st_size), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        ssize_t count = read(fd, &bytes[offset], bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += static_cast<size_t>(count);
    }
    char extra;
    ssize_t trailing;
    do { trailing = read(fd, &extra, 1); } while (trailing < 0 && errno == EINTR);
    close(fd);
    if (offset != bytes.size() || trailing != 0) return false;
    RuntimeAck parsed;
    std::string reason;
    if (!parseRuntimeAck(bytes, parsed) || !validateRuntimeAck(parsed, reason) || parsed.engine != engine) return false;
    out = std::move(parsed);
    return true;
}

inline bool runtimeAckAdvances(const RuntimeAck &previous, const RuntimeAck &next) {
    if (!runtimeAckHasLiveIdentity(next)) return false;
    if (previous.schemaVersion != 2 || previous.bootId != next.bootId) return true;
    if (next.configGeneration < previous.configGeneration) return false;
    bool sameProcess = next.pid == previous.pid && next.processStartTicks == previous.processStartTicks;
    bool sameSnapshot = next.configGeneration == previous.configGeneration &&
        next.configHash == previous.configHash && next.requestId == previous.requestId;
    if (sameProcess && sameSnapshot) {
        if (next.timestamp < previous.timestamp || previous.eventType == AckEventType::APPLY_FAILED) return false;
        return static_cast<int>(next.eventType) > static_cast<int>(previous.eventType);
    }
    // A newer snapshot/process may restart progression, but delayed old sessions may not.
    if (next.timestamp < previous.timestamp) return false;
    if (sameProcess) return next.configGeneration > previous.configGeneration ||
        (next.configGeneration == previous.configGeneration && next.requestId != previous.requestId);
    return !runtimeAckHasLiveIdentity(previous) || next.processStartTicks > previous.processStartTicks ||
        (next.processStartTicks == previous.processStartTicks && next.timestamp > previous.timestamp);
}

inline bool writeRuntimeAckDirect(const std::string &runtime_dir, const RuntimeAck &ack) {
    std::string err;
    if (!validateRuntimeAck(ack, err) || !runtimeAckHasLiveIdentity(ack)) {
        return false;
    }

    std::string serialized = ack.toJson().dump(2);
    if (serialized.empty() || serialized.size() > 65536) return false;
    int dir = openRuntimeAckDirectory(runtime_dir, true);
    if (dir < 0) return false;
    int locked;
    do { locked = flock(dir, LOCK_EX); } while (locked != 0 && errno == EINTR);
    if (locked != 0) { close(dir); return false; }
    RuntimeAck previous;
    if (!runtimeAckHasLiveIdentity(ack) ||
        (readRuntimeAckAt(dir, ack.engine, previous) && !runtimeAckAdvances(previous, ack))) {
        close(dir); return false;
    }
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
    int locked;
    do { locked = flock(dir, LOCK_SH); } while (locked != 0 && errno == EINTR);
    bool ok = locked == 0 && readRuntimeAckAt(dir, engine, out);
    close(dir);
    return ok;
}

} // namespace kaorios
