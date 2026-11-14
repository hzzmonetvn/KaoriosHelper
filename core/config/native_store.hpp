#pragma once
#include <string>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <cerrno>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <atomic>
#include <cstdlib>
#include "../../third_party/json/nlohmann/json.hpp"

namespace kaorios {
// All paths below are single leaf names relative to the pinned, private directory.
class NativeStore {
    int dir_ = -1, lock_ = -1;
    static bool trusted(const struct stat &s) {
        return s.st_uid == geteuid() && !(s.st_mode & 0022);
    }
public:
    explicit NativeStore(const std::string &path) {
        dir_ = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat s{};
        if (dir_ < 0 || fstat(dir_, &s) || !trusted(s)) {
            if (dir_ >= 0) close(dir_);
            fail("Untrusted data directory");
        }
        lock_ = openat(dir_, ".config.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        if (lock_ < 0 || fstat(lock_, &s) || !S_ISREG(s.st_mode) || !trusted(s) || s.st_nlink != 1 || flock(lock_, LOCK_EX)) {
            if (lock_ >= 0) close(lock_);
            close(dir_);
            fail("Configuration lock failed");
        }
        try { recover(); }
        catch (...) { close(lock_); close(dir_); throw; }
    }
    ~NativeStore() { if (lock_ >= 0) close(lock_); if (dir_ >= 0) close(dir_); }
    NativeStore(const NativeStore&) = delete;
    static void fail(const char *message) { throw std::runtime_error(message); }
    void check(const std::string &name) {
        if (name.empty() || name == "." || name == ".." ||
            name.find('/') != std::string::npos || name.find('\0') != std::string::npos)
            fail("Invalid configuration leaf name");
        struct stat s{};
        if (fstatat(dir_, name.c_str(), &s, AT_SYMLINK_NOFOLLOW)) {
            if (errno == ENOENT) return;
            fail("Target inspection failed");
        }
        if (!S_ISREG(s.st_mode) || !trusted(s) || s.st_nlink != 1) fail("Untrusted configuration file");
    }
    bool exists(const std::string &name) {
        check(name);
        struct stat s{};
        if (fstatat(dir_, name.c_str(), &s, AT_SYMLINK_NOFOLLOW) == 0) return true;
        if (errno != ENOENT) fail("Target inspection failed");
        return false;
    }
    std::string read(const std::string &name, bool optional = false, size_t limit = 16*1024*1024) {
        check(name);
        int fd = openat(dir_, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { if (optional && errno == ENOENT) return ""; fail("Configuration read failed"); }
        struct stat s{};
        if (fstat(fd, &s) || !S_ISREG(s.st_mode) || !trusted(s) || s.st_nlink != 1 || s.st_size < 0 || static_cast<uint64_t>(s.st_size) > limit) {
            close(fd); fail("Untrusted configuration file");
        }
        std::string out; char buf[8192]; ssize_t n;
        while ((n = ::read(fd, buf, sizeof(buf))) != 0) {
            if (n < 0) { if (errno == EINTR) continue; close(fd); fail("Configuration read failed"); }
            out.append(buf, n);
            if (out.size() > limit) { close(fd); fail("Configuration too large"); }
        }
        close(fd); return out;
    }
    void write(const std::string &name, const std::string &value) {
        check(name);
        static std::atomic<unsigned> counter{0};
        std::string tmp; int fd = -1;
        for (unsigned i=0; i<100 && fd<0; ++i) {
            tmp = ".staging." + std::to_string(getpid()) + "." + std::to_string(++counter);
            fd = openat(dir_, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0 && errno != EEXIST) fail("Staging creation failed");
        }
        if (fd < 0) fail("Staging creation failed");
        bool ok = fchmod(fd, 0600) == 0;
        size_t pos = 0;
        while (ok && pos < value.size()) {
            ssize_t n = ::write(fd, value.data()+pos, value.size()-pos);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            pos += n;
        }
        if (ok) ok = fsync(fd) == 0;
        if (close(fd)) ok = false;
        if (ok) ok = renameat(dir_, tmp.c_str(), dir_, name.c_str()) == 0;
        if (!ok) { unlinkat(dir_, tmp.c_str(), 0); fail("Configuration publication failed"); }
        if (fsync(dir_)) fail("Directory sync failed");
    }
    void remove(const std::string &name) {
        check(name);
        if (unlinkat(dir_, name.c_str(), 0) && errno != ENOENT) fail("Configuration removal failed");
        if (fsync(dir_)) fail("Directory sync failed");
    }

    using Changes = std::map<std::string, std::optional<std::string>>;
    // The journal is the durable commit decision. Cooperating readers hold this
    // lock and replay it before observing files; arbitrary unlocked readers do not.
    void transaction(const Changes &changes) {
        if (changes.empty()) return;
        validateChanges(changes);
        nlohmann::json entries = nlohmann::json::object();
        for (const auto &[name, value] : changes) {
            if (value) entries[name] = *value;
            else entries[name] = nullptr;
        }
        const auto journal = nlohmann::json{{"schemaVersion", 1}, {"files", entries}}.dump();
        if (journal.size() > journalLimit) fail("Configuration transaction too large");
        write(journalName, journal);
        apply(changes);
    }
private:
    static constexpr const char *journalName = ".native-transaction.json";
    static constexpr size_t journalLimit = 128*1024*1024;
    static bool transactionLeaf(const std::string &name) {
        for (const auto *filename : {"hma.json", "pif.json", "keybox.xml", "spoof.json", "kaorios.json"})
            if (name == filename || name == std::string(filename) + ".lkg") return true;
        for (const auto *engine : {"hma", "pif", "tee", "copg", "kaorios"})
            for (const auto *suffix : {".stored_gen", ".requested_gen", ".request.json", ".reload_pending", ".verified", ".enabled", ".disabled"})
                if (name == std::string(engine) + suffix) return true;
        return false;
    }
    void validateChanges(const Changes &changes) {
        if (changes.empty() || changes.size() > 64) fail("Invalid configuration transaction");
        size_t total = 0;
        for (const auto &[name, value] : changes) {
            if (!transactionLeaf(name)) fail("Invalid transaction target");
            check(name);
            if (value) {
                if (value->size() > 16*1024*1024) fail("Configuration too large");
                total += value->size();
            }
        }
        if (total > 32*1024*1024) fail("Configuration transaction too large");
    }
    void apply(const Changes &changes) {
        size_t applied = 0;
        for (const auto &[name, value] : changes) {
#ifdef KAORIOS_TEST_FAULTS
            const char *fault = std::getenv("KAORIOS_TEST_TX_STOP_AFTER");
            if (fault && std::to_string(applied) == fault) _exit(86);
#endif
            if (value) write(name, *value);
            else remove(name);
            ++applied;
        }
        remove(journalName);
    }
    void recover() {
        const auto text = read(journalName, true, journalLimit);
        if (text.empty()) {
            if (exists(journalName)) fail("Invalid configuration journal");
            return;
        }
        const auto journal = nlohmann::json::parse(text, nullptr, false);
        if (!journal.is_object() || journal.size() != 2 || !journal.contains("schemaVersion") ||
            !journal["schemaVersion"].is_number_integer() || journal["schemaVersion"] != 1 ||
            !journal.contains("files") || !journal["files"].is_object()) fail("Invalid configuration journal");
        Changes changes;
        for (auto it = journal["files"].begin(); it != journal["files"].end(); ++it) {
            if (it.value().is_null()) changes[it.key()] = std::nullopt;
            else if (it.value().is_string()) changes[it.key()] = it.value().get<std::string>();
            else fail("Invalid configuration journal");
        }
        validateChanges(changes);
        apply(changes);
    }
};
inline bool validateNativeJson(const std::string &engine, const std::string &text) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    if ((engine == "pif" || engine == "copg") && j.contains("enabled") && !j["enabled"].is_boolean()) return false;
    if (engine == "kaorios" && j.contains("schemaVersion") &&
        (!j["schemaVersion"].is_number_integer() || j["schemaVersion"] != 1)) return false;
    if (engine == "hma") {
        if (j.contains("configVersion") && !j["configVersion"].is_number_integer()) return false;
        for (const auto *k : {"templates", "settingsTemplates", "scope"})
            if (j.contains(k) && !j[k].is_object()) return false;
    }
    if (engine == "pif") {
        for (const auto *k : {"MANUFACTURER", "MODEL", "FINGERPRINT", "BRAND", "PRODUCT", "DEVICE"})
            if (j.contains(k) && !j[k].is_string()) return false;
        for (const auto *k : {"spoofBuild", "spoofProps", "spoofProvider", "spoofSignature"})
            if (j.contains(k) && !j[k].is_boolean() && !j[k].is_number_integer()) return false;
    }
    if (engine == "copg") {
        if (j.contains("cpu_spoof")) {
            const auto &cpu = j["cpu_spoof"];
            if (!cpu.is_object()) return false;
            for (const auto *k : {"blacklist", "cpu_only_packages"})
                if (cpu.contains(k) && !cpu[k].is_array()) return false;
        }
        for (const auto *section : {"profiles", "packages"}) {
            if (!j.contains(section)) continue;
            const auto &profiles = j[section];
            if (std::string(section) == "packages" && profiles.is_array()) {
                for (const auto &pkg : profiles) if (!pkg.is_string()) return false;
                continue;
            }
            if (!profiles.is_object()) return false;
            for (const auto &profile : profiles) {
                if (!profile.is_object()) return false;
                for (const auto *flag : {"enabled", "device_spoof", "cpu_spoof", "plt_hook", "android_id_spoof"})
                    if (profile.contains(flag) && !profile[flag].is_boolean()) return false;
                for (const auto *field : {"model", "brand", "manufacturer", "device", "product", "fingerprint", "release", "serial", "android_id"})
                    if (profile.contains(field) && !profile[field].is_string()) return false;
                if (profile.contains("sdk_int") && (!profile["sdk_int"].is_number_integer() ||
                    profile["sdk_int"] < 0 || profile["sdk_int"] > std::numeric_limits<int>::max())) return false;
                if (profile.contains("props")) {
                    if (!profile["props"].is_object()) return false;
                    for (const auto &value : profile["props"]) if (!value.is_string()) return false;
                }
            }
        }
        for (auto it=j.begin(); it!=j.end(); ++it) {
            const auto &key=it.key();
            if (key.rfind("PACKAGES_", 0) != 0) continue;
            bool device = key.size() >= 7 && key.compare(key.size()-7, 7, "_DEVICE") == 0;
            if (device ? !it.value().is_object() : !it.value().is_array()) return false;
        }
    }
    return true;
}
}
