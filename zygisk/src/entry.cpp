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

    char buf[512] = {0};
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

    if (command == "mount_spoof") {
        const char *spoof_path = "/data/adb/modules/kaorioshelper/data/cpuinfo_spoof";
        if (access(spoof_path, F_OK) != 0) {
            spoof_path = "/data/adb/modules/COPG/cpuinfo_spoof";
        }
        if (access(spoof_path, F_OK) == 0) {
            result = run_in_target_ns([&]() -> int {
                umount2("/proc/cpuinfo", MNT_DETACH);
                if (mount(spoof_path, "/proc/cpuinfo", nullptr, MS_BIND, nullptr) == 0) {
                    LOGI("KaoriosHelper companion: mounted cpuinfo_spoof -> /proc/cpuinfo for pid=%d", target_pid);
                    return 0;
                } else {
                    LOGW("KaoriosHelper companion: mount(MS_BIND) failed: %s", strerror(errno));
                    return -1;
                }
            });
        } else {
            LOGW("KaoriosHelper companion: cpuinfo_spoof file not found at %s", spoof_path);
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
    } else {
        LOGW("KaoriosHelper companion: unrecognized command '%s'", command.c_str());
    }

    write(client_socket, &result, sizeof(result));
    close(client_socket);
}

} // namespace kaorios

REGISTER_ZYGISK_MODULE(kaorios::KaoriosHelperModule)
REGISTER_ZYGISK_COMPANION(kaorios::companion_handler)
