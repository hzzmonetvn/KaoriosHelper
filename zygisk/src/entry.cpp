/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#include "zygisk.hpp"
#include "dispatcher.hpp"
#include "logging.hpp"
#include "hma_engine.hpp"
#include "pif_engine.hpp"
#include "tee_engine.hpp"
#include "copg_engine.hpp"
#include "ack_protocol.hpp"
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <signal.h>
#include <sched.h>
#include <fcntl.h>
#include <cerrno>

namespace kaorios {

class KaoriosHelperModule : public zygisk::ModuleBase {
public:
    KaoriosHelperModule() {
        // Register all engines into unified dispatcher
        dispatcher_.registerEngine(std::make_unique<HmaEngine>());
        dispatcher_.registerEngine(std::make_unique<PifEngine>());
        dispatcher_.registerEngine(std::make_unique<TeeEngine>());
        dispatcher_.registerEngine(std::make_unique<CopgEngine>());
    }

    virtual void onLoad(zygisk::Api *api, JNIEnv *env) override {
        dispatcher_.onLoad(api, env);
    }

    virtual void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        dispatcher_.preAppSpecialize(args);
    }

    virtual void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        dispatcher_.postAppSpecialize(args);
    }

    virtual void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        dispatcher_.preServerSpecialize(args);
    }

    virtual void postServerSpecialize(const zygisk::ServerSpecializeArgs *args) override {
        dispatcher_.postServerSpecialize(args);
    }

private:
    KaoriosDispatcher dispatcher_;
};

static void companion_handler(int client_socket) {
    LOGD("KaoriosHelper root companion socket connected: %d", client_socket);

    // Acquire peer credentials to locate target client PID and its mount namespace
    struct ucred cr{};
    socklen_t len = sizeof(cr);
    pid_t target_pid = -1;
    if (getsockopt(client_socket, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0) {
        target_pid = cr.pid;
        LOGD("KaoriosHelper companion: peer pid=%d uid=%d gid=%d", cr.pid, cr.uid, cr.gid);
    } else {
        LOGW("KaoriosHelper companion: getsockopt(SO_PEERCRED) failed: %s", strerror(errno));
    }

    char buf[4096] = {0};
    ssize_t bytes_read = read(client_socket, buf, sizeof(buf) - 1);
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }
    buf[bytes_read] = '\0';
    std::string command(buf);

    int result = -1;

    // Helper to execute operations inside target mount namespace
    auto run_in_target_ns = [&](auto &&fn) -> int {
        if (target_pid <= 1) {
            LOGW("KaoriosHelper companion: target_pid <= 1 (%d), refusing to operate", target_pid);
            return -1;
        }

        // Check caller identity: UID 0 indicates unspecialized Zygote process
        if (cr.uid == 0) {
            LOGW("KaoriosHelper companion: caller uid is 0 (unspecialized Zygote child); refusing namespace operations");
            return -1;
        }

        // Check if target process is still alive (guard against PID reuse / race)
        if (kill(target_pid, 0) != 0) {
            LOGW("KaoriosHelper companion: target pid=%d is no longer alive: %s", target_pid, strerror(errno));
            return -1;
        }

        char ns_path[64];
        snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/mnt", target_pid);
        int target_ns = open(ns_path, O_RDONLY | O_CLOEXEC);
        if (target_ns < 0) {
            LOGW("KaoriosHelper companion: failed to open target mnt_ns %s: %s", ns_path, strerror(errno));
            return -1;
        }

        struct stat target_st{};
        if (fstat(target_ns, &target_st) != 0) {
            LOGW("KaoriosHelper companion: failed to stat target mnt_ns: %s", strerror(errno));
            close(target_ns);
            return -1;
        }

        // Check if target mount namespace is shared with PID 1 (init / system root)
        int root_ns = open("/proc/1/ns/mnt", O_RDONLY | O_CLOEXEC);
        if (root_ns >= 0) {
            struct stat root_st{};
            if (fstat(root_ns, &root_st) == 0 && target_st.st_ino == root_st.st_ino) {
                LOGE("KaoriosHelper companion: Refusing mount operation: target pid=%d shares mount namespace with PID 1/root (inode=%lu). App specialization has not unshared mount namespace!",
                     target_pid, (unsigned long)target_st.st_ino);
                close(target_ns);
                close(root_ns);
                return -1;
            }
            close(root_ns);
        }

        int self_ns = open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
        if (self_ns < 0) {
            LOGW("KaoriosHelper companion: failed to open self mnt_ns: %s", strerror(errno));
            close(target_ns);
            return -1;
        }

        struct stat self_st{};
        if (fstat(self_ns, &self_st) == 0 && target_st.st_ino == self_st.st_ino) {
            LOGE("KaoriosHelper companion: Refusing mount operation: target pid=%d shares mount namespace with companion daemon (inode=%lu)",
                 target_pid, (unsigned long)target_st.st_ino);
            close(target_ns);
            close(self_ns);
            return -1;
        }

        if (setns(target_ns, CLONE_NEWNS) != 0) {
            LOGW("KaoriosHelper companion: setns to target failed: %s", strerror(errno));
            close(target_ns);
            close(self_ns);
            return -1;
        }
        close(target_ns);

        // Make /proc mount propagation private/slave to prevent leaking bind mounts outside target
        mount(nullptr, "/proc", nullptr, MS_SLAVE | MS_REC, nullptr);

        int r = fn();

        if (setns(self_ns, CLONE_NEWNS) != 0) {
            LOGE("KaoriosHelper companion: FATAL failed to restore self mnt_ns: %s. Terminating companion immediately to prevent namespace corruption.", strerror(errno));
            close(self_ns);
            _exit(1);
        }
        close(self_ns);
        return r;
    };

    if (command == "mount_spoof" || command.rfind("mount_cpu:", 0) == 0) {
        std::string spoof_path = "/data/adb/modules/kaorioshelper/data/cpuinfo_spoof";
        if (command.rfind("mount_cpu:", 0) == 0) {
            std::string rem = command.substr(10);
            auto colon = rem.find(':');
            if (colon != std::string::npos) {
                std::string pid_str = rem.substr(0, colon);
                std::string custom_src = rem.substr(colon + 1);
                pid_t req_pid = std::atoi(pid_str.c_str());
                if (req_pid > 1 && (cr.uid == 0 || target_pid == req_pid)) {
                    target_pid = req_pid;
                }
                if (!custom_src.empty()) {
                    spoof_path = custom_src;
                }
            }
        }
        if (access(spoof_path.c_str(), F_OK) != 0) {
            spoof_path = "/data/adb/modules/COPG/cpuinfo_spoof";
        }
        if (access(spoof_path.c_str(), F_OK) == 0) {
            result = run_in_target_ns([&]() -> int {
                umount2("/proc/cpuinfo", MNT_DETACH);
                if (mount(spoof_path.c_str(), "/proc/cpuinfo", nullptr, MS_BIND, nullptr) == 0) {
                    LOGI("KaoriosHelper companion: mounted %s -> /proc/cpuinfo for pid=%d", spoof_path.c_str(), target_pid);
                    return 0;
                } else {
                    LOGW("KaoriosHelper companion: mount(MS_BIND) failed: %s", strerror(errno));
                    return -1;
                }
            });
        } else {
            LOGW("KaoriosHelper companion: cpuinfo_spoof file not found at %s", spoof_path.c_str());
        }
    } else if (command == "unmount_spoof") {
        result = run_in_target_ns([&]() -> int {
            int uerr = umount2("/proc/cpuinfo", MNT_DETACH);
            if (uerr == 0) {
                LOGI("KaoriosHelper companion: unmounted /proc/cpuinfo for pid=%d", target_pid);
                return 0;
            } else {
                LOGD("KaoriosHelper companion: umount2 failed (or wasn't mounted): %s", strerror(errno));
                return uerr;
            }
        });
    } else if (command.rfind("ack:", 0) == 0) {
        // Safe privileged runtime acknowledgement submission
        std::string payload = command.substr(4);
        auto j = nlohmann::json::parse(payload, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            RuntimeAck ack;
            if (RuntimeAck::fromJson(j, ack)) {
                // Securely override PID & UID with peer credentials from kernel
                ack.pid = target_pid;
                ack.uid = cr.uid;

                // Authenticate process name from /proc/<pid>/cmdline
                char cmdline_path[64];
                snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", target_pid);
                int cmd_fd = open(cmdline_path, O_RDONLY | O_CLOEXEC);
                if (cmd_fd >= 0) {
                    char cmd_buf[256] = {0};
                    ssize_t n = read(cmd_fd, cmd_buf, sizeof(cmd_buf) - 1);
                    close(cmd_fd);
                    if (n > 0) {
                        cmd_buf[n] = '\0';
                        ack.processName = std::string(cmd_buf);
                    }
                }

                if (ack.timestamp == 0) {
                    struct timespec ts{};
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ack.timestamp = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
                }

                std::string reason;
                if (!validateRuntimeAck(ack, reason)) {
                    LOGW("KaoriosHelper companion: rejected unauthorized runtime ack from pid=%d uid=%d (%s): %s",
                         target_pid, cr.uid, ack.processName.c_str(), reason.c_str());
                    result = -1;
                } else {
                    std::string runtime_dir = "/data/adb/modules/kaorioshelper/data/runtime";
                    RuntimeAck prev_ack;
                    if (readRuntimeAck(runtime_dir, ack.engine, prev_ack)) {
                        if (ack.configGeneration < prev_ack.configGeneration && ack.configGeneration != 0) {
                            LOGW("KaoriosHelper companion: rejected stale ack (gen %lu < prev gen %lu) for %s",
                                 (unsigned long)ack.configGeneration, (unsigned long)prev_ack.configGeneration, ack.engine.c_str());
                            result = -2;
                            write(client_socket, &result, sizeof(result));
                            close(client_socket);
                            return;
                        }
                    }

                    if (writeRuntimeAckDirect(runtime_dir, ack)) {
                        LOGI("KaoriosHelper companion: authenticated and committed runtime ack for %s (%s, pid=%d, uid=%d, gen=%lu)",
                             ack.engine.c_str(), ack.processName.c_str(), ack.pid, ack.uid, (unsigned long)ack.configGeneration);
                        result = 0;
                    } else {
                        LOGW("KaoriosHelper companion: writeRuntimeAckDirect failed for %s", ack.engine.c_str());
                    }
                }
            }
        }
    } else {
        LOGW("KaoriosHelper companion: unrecognized command '%s'", command.c_str());
    }

    write(client_socket, &result, sizeof(result));
    close(client_socket);
}

} // namespace kaorios

REGISTER_ZYGISK_MODULE(kaorios::KaoriosHelperModule)
REGISTER_ZYGISK_COMPANION(kaorios::companion_handler)
