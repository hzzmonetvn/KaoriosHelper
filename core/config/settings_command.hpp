#pragma once
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <csignal>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace kaorios {
// Never invoke a shell or expose settings values in subprocess diagnostics.
inline std::string readGlobalSetting(const std::string &key,
        std::chrono::steady_clock::time_point deadline) {
    const char *binary = "/system/bin/settings";
#ifdef KAORIOS_TEST_FAULTS
    if (const char *fixture = std::getenv("KAORIOS_TEST_SETTINGS_BIN")) binary = fixture;
#endif
    int output[2];
    if (pipe2(output, O_CLOEXEC)) throw std::runtime_error("Settings transport creation failed");
    const pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) < 0) _exit(126);
        close(output[1]);
        int sink = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (sink < 0 || dup2(sink, STDERR_FILENO) < 0 || dup2(sink, STDIN_FILENO) < 0) _exit(126);
        if (sink > STDERR_FILENO) close(sink);
        execl(binary, binary, "get", "global", key.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    close(output[1]);
    if (pid < 0) { close(output[0]); throw std::runtime_error("Settings command start failed"); }
    setpgid(pid, pid);
    int flags = fcntl(output[0], F_GETFL);
    bool failed = flags < 0 || fcntl(output[0], F_SETFL, flags | O_NONBLOCK) < 0;
    bool eof = false, exited = false;
    int status = 0;
    std::string value;
    while (!failed && (!eof || !exited)) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { failed = true; break; }
        pollfd fd{eof ? -1 : output[0], POLLIN | POLLHUP, 0};
        int ready = poll(&fd, 1, static_cast<int>(std::min<int64_t>(remaining, 50)));
        if (ready < 0 && errno != EINTR) { failed = true; break; }
        if (ready > 0) {
            char buffer[8192];
            for (;;) {
                ssize_t n = read(output[0], buffer, sizeof(buffer));
                if (n == 0) { eof = true; break; }
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (errno != EAGAIN && errno != EWOULDBLOCK) failed = true;
                    break;
                }
                value.append(buffer, static_cast<size_t>(n));
                if (value.size() > 16*1024*1024) { failed = true; break; }
                if (std::chrono::steady_clock::now() >= deadline) { failed = true; break; }
            }
        }
        if (!exited) {
            const pid_t result = waitpid(pid, &status, WNOHANG);
            exited = result == pid;
            if (result < 0 && errno != EINTR) failed = true;
        }
    }
    close(output[0]);
    if (failed) {
        kill(-pid, SIGKILL);
        if (!exited) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
        throw std::runtime_error("Settings command failed or exceeded bounds");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) throw std::runtime_error("Settings command failed");
    // settings prints exactly one record terminator, not part of the stored value.
    if (!value.empty() && value.back() == '\n') value.pop_back();
    return value;
}
inline std::map<std::string, std::string> captureGlobalSettings(const std::vector<std::string> &keys) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::map<std::string, std::string> first;
    size_t total = 0;
    for (const auto &key : keys) {
        auto value = readGlobalSetting(key, deadline);
        total += value.size();
        if (total > 32*1024*1024) throw std::runtime_error("Settings snapshot too large");
        first.emplace(key, std::move(value));
    }
    // A bounded second read detects changes during capture, without a watcher.
    for (const auto &[key, value] : first)
        if (readGlobalSetting(key, deadline) != value) throw std::runtime_error("Settings changed during synchronization; retry explicitly");
    return first;
}
}
