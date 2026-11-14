#pragma once
#include <charconv>
#include <cstdint>
#include <string>
#include "native_store.hpp"
#include "../../include/ack_protocol.hpp"

namespace kaorios {
struct RuntimeConfigSnapshot {
    std::string bytes;
    std::string configHash;
    std::string requestId;
    std::string bootId;
    uint64_t generation = 0;
    uint64_t notBeforeMillis = 0;

    bool sameIdentity(const RuntimeConfigSnapshot &other) const {
        return bytes == other.bytes && generation == other.generation &&
            requestId == other.requestId && bootId == other.bootId &&
            notBeforeMillis == other.notBeforeMillis;
    }
};

inline uint64_t runtimeSnapshotGeneration(const std::string &text) {
    if (text.empty()) return 0;
    uint64_t generation = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), generation);
    if (result.ec != std::errc{} || result.ptr == text.data() || generation == 0) return 0;
    for (const char *end = result.ptr; end != text.data() + text.size(); ++end)
        if (*end != ' ' && *end != '\r' && *end != '\n' && *end != '\t') return 0;
    return generation;
}

inline RuntimeConfigSnapshot readRuntimeConfigSnapshot(const std::string &dataDir,
        const std::string &engine, const std::string &filename) {
    std::string canonical;
    if (engine == "hma" || engine == "pif") canonical = engine + ".json";
    else if (engine == "copg") canonical = "spoof.json";
    else throw std::runtime_error("Invalid runtime snapshot engine");
    if (filename != canonical && !(engine == "copg" && filename == "COPG.json"))
        throw std::runtime_error("Invalid runtime snapshot filename");

    NativeStore store(dataDir);
    RuntimeConfigSnapshot snapshot;
    snapshot.bytes = store.read(filename, engine == "copg" && filename == canonical);
    const bool legacyFallback = engine == "copg" && filename == canonical && snapshot.bytes.empty();
    if (legacyFallback) snapshot.bytes = store.read("COPG.json");
    snapshot.configHash = configSha256(snapshot.bytes);
    // Compatibility payloads remain readable but never inherit native metadata.
    if (filename != canonical || legacyFallback) return snapshot;
    const uint64_t stored = runtimeSnapshotGeneration(store.read(engine + ".stored_gen", true));
    const uint64_t requested = runtimeSnapshotGeneration(store.read(engine + ".requested_gen", true));
    const auto request = nlohmann::json::parse(store.read(engine + ".request.json", true), nullptr, false);
    if (stored == 0 || stored != requested || !request.is_object() ||
        !request.contains("requestId") || !request["requestId"].is_string() ||
        !request.contains("bootId") || !request["bootId"].is_string() ||
        !request.contains("notBeforeMillis") || !request["notBeforeMillis"].is_number_unsigned()) return snapshot;
    const auto token = request["requestId"].get<std::string>();
    if (token.size() != 32) return snapshot;
    for (char c : token)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return snapshot;
    const auto boot = request["bootId"].get<std::string>();
    const uint64_t notBefore = request["notBeforeMillis"].get<uint64_t>();
    const uint64_t now = runtimeAckNowMillis();
    if (boot.empty() || boot != runtimeAckBootId() || notBefore == 0 || notBefore > now) return snapshot;
    snapshot.generation = stored;
    snapshot.requestId = token;
    snapshot.bootId = boot;
    snapshot.notBeforeMillis = notBefore;
    return snapshot;
}

inline bool bindRuntimeAckToSnapshot(RuntimeAck &ack, const RuntimeConfigSnapshot &snapshot) {
    // Use the captured request, never metadata re-read after hooks ran.
    if (snapshot.generation == 0 || snapshot.bootId != runtimeAckBootId() ||
        snapshot.configHash != configSha256(snapshot.bytes)) return false;
    if (!bindRuntimeAckToCurrentProcess(ack, snapshot.generation, snapshot.bytes, snapshot.requestId)) return false;
    return ack.timestamp >= snapshot.notBeforeMillis;
}
}
