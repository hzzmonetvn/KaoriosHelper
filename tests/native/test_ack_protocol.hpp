/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */
#pragma once

#include "ack_protocol.hpp"
#include <cassert>
#include <filesystem>
#include <sys/wait.h>

inline void test_ack_protocol_identity(const std::string &directory) {
    using namespace kaorios;
    assert(configSha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(configSha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(configSha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    assert(configSha256(std::string(1000000, 'a')) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    const std::string token(32, 'a');
    RuntimeAck ack;
    ack.engine = "copg";
    assert(!bindRuntimeAckToCurrentProcess(ack, 1, "{}"));
    assert(!bindRuntimeAckToCurrentProcess(ack, 0, "{}", token));
    assert(bindRuntimeAckToCurrentProcess(ack, 1, "{}", token));
    assert(ack.pid == getpid());
    assert(ack.uid == geteuid());
    assert(runtimeAckMatchesCurrent(ack, "copg", 1, "{}", token));
    assert(!runtimeAckMatchesCurrent(ack, "copg", 1, "{}"));
    assert(!runtimeAckMatchesCurrent(ack, "copg", 1, "{}", std::string(32, 'b')));
    assert(!runtimeAckMatchesCurrent(ack, "pif", 1, "{}", token));
    assert(!runtimeAckMatchesCurrent(ack, "copg", 2, "{}", token));
    assert(!runtimeAckMatchesCurrent(ack, "copg", 1, "{ }", token));
    auto valid = ack.toJson();
    RuntimeAck parsed;
    assert(RuntimeAck::fromJson(valid, parsed));
    assert(parseRuntimeAck(valid.dump(), parsed));
    assert(!parseRuntimeAck("{\"uid\":0," + valid.dump().substr(1), parsed));
    assert(!parseRuntimeAck(valid.dump() + "{}", parsed));
    assert(!parseRuntimeAck(std::string(65537, 'x'), parsed));
    assert(!parseRuntimeAck(std::string(10000, '[') + "0" + std::string(10000, ']'), parsed));
    for (const auto &item : valid.items()) {
        auto missing = valid;
        missing.erase(item.key());
        assert(!RuntimeAck::fromJson(missing, parsed));
        for (const auto &wrong : {nlohmann::json(nullptr), nlohmann::json(true), nlohmann::json::array()}) {
            auto malformed = valid;
            malformed[item.key()] = wrong;
            assert(!RuntimeAck::fromJson(malformed, parsed));
        }
    }
    for (const char *field : {"pid", "uid", "configGeneration", "processStartTicks", "timestamp"}) {
        auto malformed = valid;
        malformed[field] = -1;
        assert(!RuntimeAck::fromJson(malformed, parsed));
        malformed[field] = 1.0;
        assert(!RuntimeAck::fromJson(malformed, parsed));
    }
    for (const char *field : {"pid", "initResult", "errorCode"}) {
        auto malformed = valid;
        malformed[field] = std::numeric_limits<uint64_t>::max();
        assert(!RuntimeAck::fromJson(malformed, parsed));
    }
    auto unknown = valid;
    unknown["eventType"] = "UNKNOWN_SUCCESS";
    assert(!RuntimeAck::fromJson(unknown, parsed));
    auto legacy = valid;
    legacy["schemaVersion"] = legacy["protocolVersion"] = 1;
    legacy.erase("bootId");
    legacy.erase("processStartTicks");
    legacy.erase("configHash");
    legacy.erase("requestId");
    assert(RuntimeAck::fromJson(legacy, parsed));
    assert(!runtimeAckMatchesCurrent(parsed, "copg", 1, "{}", token));
    auto damaged = ack;
    damaged.bootId[0] = damaged.bootId[0] == '0' ? '1' : '0';
    assert(!runtimeAckHasLiveIdentity(damaged));
    damaged = ack;
    ++damaged.processStartTicks;
    assert(!runtimeAckHasLiveIdentity(damaged));
    damaged = ack;
    ++damaged.uid;
    assert(!runtimeAckHasLiveIdentity(damaged));
    damaged = ack;
    damaged.processName += ".wrong";
    assert(!runtimeAckHasLiveIdentity(damaged));
    damaged = ack;
    damaged.pid = std::numeric_limits<pid_t>::max();
    assert(!runtimeAckHasLiveIdentity(damaged));
    damaged = ack;
    damaged.timestamp = 1;
    assert(!runtimeAckHasLiveIdentity(damaged));
    damaged = ack;
    damaged.timestamp = runtimeAckNowMillis() + 60000;
    assert(!runtimeAckHasLiveIdentity(damaged));

    // A real child has a distinct start/session identity and becomes stale at exit.
    int payload[2], release[2];
    assert(pipe(payload) == 0 && pipe(release) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(payload[0]); close(release[1]);
        RuntimeAck childAck;
        childAck.engine = "copg";
        if (!bindRuntimeAckToCurrentProcess(childAck, 1, "{}", token)) _exit(1);
        auto serialized = childAck.toJson().dump();
        if (write(payload[1], serialized.data(), serialized.size()) != static_cast<ssize_t>(serialized.size())) _exit(1);
        close(payload[1]);
        char byte;
        if (read(release[0], &byte, 1) != 1) _exit(1);
        _exit(0);
    }
    close(payload[1]); close(release[0]);
    std::string bytes;
    char buffer[4096];
    ssize_t count;
    while ((count = read(payload[0], buffer, sizeof(buffer))) > 0) bytes.append(buffer, static_cast<size_t>(count));
    close(payload[0]);
    RuntimeAck childAck;
    assert(RuntimeAck::fromJson(nlohmann::json::parse(bytes), childAck));
    assert(runtimeAckHasLiveIdentity(childAck));
    assert(runtimeAckMatchesCurrent(childAck, "copg", 1, "{}", token));
    assert(runtimeAckAdvances(ack, childAck));
    if (childAck.processStartTicks > ack.processStartTicks || childAck.timestamp > ack.timestamp)
        assert(!runtimeAckAdvances(childAck, ack));
    assert(write(release[1], "x", 1) == 1);
    close(release[1]);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(!runtimeAckHasLiveIdentity(childAck));

    const std::string runtime = directory + "/ack_ordered";
    assert(writeRuntimeAckDirect(runtime, ack));
    assert(!writeRuntimeAckDirect(runtime, ack));
    ack.eventType = AckEventType::HOOK_INITIALIZED;
    assert(writeRuntimeAckDirect(runtime, ack));
    auto delayed = ack;
    delayed.eventType = AckEventType::CONFIG_LOADED;
    assert(!writeRuntimeAckDirect(runtime, delayed));
    ack.eventType = AckEventType::HOOK_OBSERVED;
    assert(writeRuntimeAckDirect(runtime, ack));
    assert(!writeRuntimeAckDirect(runtime, ack));
    delayed = ack;
    delayed.configHash = configSha256("different");
    assert(!writeRuntimeAckDirect(runtime, delayed));
    RuntimeAck read;
    assert(readRuntimeAck(runtime, "copg", read) && read.eventType == AckEventType::HOOK_OBSERVED);
    ack.eventType = AckEventType::APPLY_FAILED;
    ack.initResult = -1;
    assert(writeRuntimeAckDirect(runtime, ack));
    ack.eventType = AckEventType::HOOK_OBSERVED;
    ack.initResult = 0;
    assert(!writeRuntimeAckDirect(runtime, ack));
    ++ack.configGeneration;
    ack.eventType = AckEventType::PROCESS_LOADED;
    assert(writeRuntimeAckDirect(runtime, ack));
    ack.requestId = std::string(32, 'b');
    assert(writeRuntimeAckDirect(runtime, ack));
    std::filesystem::remove_all(runtime);
}
