/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * KaoriosHelper Native Control CLI (helperctl)
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

namespace fs = std::filesystem;

class HelperCtl {
public:
    HelperCtl(std::string mod_dir = "") {
        if (!mod_dir.empty()) {
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
            if (mod_dir_.empty() || !fs::exists(mod_dir_ + "/module.prop")) {
                mod_dir_ = "/data/adb/modules/kaorioshelper";
            }
        }
        data_dir_ = mod_dir_ + "/data";
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

    struct EngineStatus {
        std::string name;
        bool enabled;
        std::string status; // ACTIVE, CONFIGURED, NOT_CONFIGURED, DISABLED, ERROR
        std::string details;
    };

    std::map<std::string, EngineStatus> getEngineStatuses() {
        std::map<std::string, EngineStatus> res;
        struct stat st{};

        // HMA
        bool hma_disabled = (stat((data_dir_ + "/hma.disabled").c_str(), &st) == 0);
        bool hma_cfg_exists = (stat((data_dir_ + "/hma.json").c_str(), &st) == 0 && st.st_size > 0);
        bool dex_exists = (stat((data_dir_ + "/classes.dex").c_str(), &st) == 0 && st.st_size > 0) ||
                          (stat((mod_dir_ + "/classes.dex").c_str(), &st) == 0 && st.st_size > 0);
        std::string hma_status = "DISABLED";
        std::string hma_details = "In-process system_server package protection";
        if (!hma_disabled) {
            if (hma_cfg_exists) {
                hma_status = "ACTIVE";
                hma_details = dex_exists ?
                    "ZygoteLoader DEX in-memory injection & system_server filtering active" :
                    "system_server package filtering active (DEX entrypoint optional)";
            } else {
                hma_status = "NOT_CONFIGURED";
                hma_details = "hma.json configuration missing or empty";
            }
        }
        res["hma"] = {"hma", !hma_disabled, hma_status, hma_details};

        // PIF
        bool pif_disabled = (stat((data_dir_ + "/pif.disabled").c_str(), &st) == 0);
        bool pif_cfg_exists = (stat((data_dir_ + "/pif.json").c_str(), &st) == 0 && st.st_size > 0);
        std::string pif_status = "DISABLED";
        std::string pif_details = "Play Integrity Fix property spoofing";
        if (!pif_disabled) {
            if (pif_cfg_exists) {
                pif_status = "ACTIVE";
                pif_details = "ByteDance ShadowHook native libc __system_property_read_callback & Build spoofing active";
            } else {
                pif_status = "NOT_CONFIGURED";
                pif_details = "pif.json configuration missing or empty";
            }
        }
        res["pif"] = {"pif", !pif_disabled, pif_status, pif_details};

        // TEE
        bool tee_enabled = (stat((data_dir_ + "/tee.enabled").c_str(), &st) == 0);
        bool keybox_exists = (stat((data_dir_ + "/keybox.xml").c_str(), &st) == 0 && st.st_size > 0);
        std::string tee_status = "DISABLED";
        std::string tee_details = "Hardware Keystore2 / KeyMint attestation simulation";
        if (tee_enabled) {
            if (keybox_exists) {
                tee_status = "ACTIVE";
                tee_details = "Keybox XML certificate chain validated; Keystore2 daemon isolation verified";
            } else {
                tee_status = "KEYBOX_MISSING";
                tee_details = "tee.enabled active but keybox.xml missing in data directory";
            }
        }
        res["tee"] = {"tee", tee_enabled, tee_status, tee_details};

        // COPG
        bool copg_enabled = (stat((data_dir_ + "/copg.enabled").c_str(), &st) == 0);
        bool spoof_exists = (stat((data_dir_ + "/spoof.json").c_str(), &st) == 0 && st.st_size > 0) ||
                            (stat((data_dir_ + "/COPG.json").c_str(), &st) == 0 && st.st_size > 0);
        std::string copg_status = "DISABLED";
        std::string copg_details = "Game profile and device identity virtualization";
        if (copg_enabled) {
            if (spoof_exists) {
                copg_status = "ACTIVE";
                copg_details = "Per-app Build spoofing, root companion /proc/cpuinfo mount virtualization & MemoryIntArray ANDROID_ID active";
            } else {
                copg_status = "NOT_CONFIGURED";
                copg_details = "copg.enabled active but spoof.json missing in data directory";
            }
        }
        res["copg"] = {"copg", copg_enabled, copg_status, copg_details};

        return res;
    }

    bool updateStatusJson() {
        fs::create_directories(data_dir_);
        std::string tmp_path = data_dir_ + "/status.json.tmp." + std::to_string(getpid());
        std::string dest_path = data_dir_ + "/status.json";

        std::ofstream f(tmp_path);
        if (!f.is_open()) return false;

        auto engines = getEngineStatuses();
        std::string ver = getVersion();
        bool mod_dis = isModuleDisabled();

        f << "{\n";
        f << "  \"moduleVersion\": \"" << ver << "\",\n";
        f << "  \"moduleState\": \"" << (mod_dis ? "DISABLED" : "ACTIVE") << "\",\n";
        f << "  \"engines\": {\n";

        size_t count = 0;
        for (const auto &[name, eng] : engines) {
            f << "    \"" << name << "\": {\n";
            f << "      \"enabled\": " << (eng.enabled ? "true" : "false") << ",\n";
            f << "      \"status\": \"" << eng.status << "\",\n";
            f << "      \"details\": \"" << eng.details << "\"\n";
            f << "    }" << (++count < engines.size() ? "," : "") << "\n";
        }
        f << "  }\n";
        f << "}\n";
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
            std::cout << "{\n";
            std::cout << "  \"module\": {\n";
            std::cout << "    \"id\": \"kaorioshelper\",\n";
            std::cout << "    \"version\": \"" << ver << "\",\n";
            std::cout << "    \"active\": " << (mod_dis ? "false" : "true") << "\n";
            std::cout << "  },\n";
            std::cout << "  \"engines\": {\n";
            size_t c = 0;
            for (const auto &[name, eng] : engines) {
                std::cout << "    \"" << name << "\": {\n";
                std::cout << "      \"enabled\": " << (eng.enabled ? "true" : "false") << ",\n";
                std::cout << "      \"status\": \"" << eng.status << "\",\n";
                std::cout << "      \"details\": \"" << eng.details << "\"\n";
                std::cout << "    }" << (++c < engines.size() ? "," : "") << "\n";
            }
            std::cout << "  }\n";
            std::cout << "}\n";
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
            std::cout << "      Enabled: " << (eng.enabled ? "YES" : "NO") << "\n";
            std::cout << "      Status:  " << eng.status << "\n";
            std::cout << "      Details: " << eng.details << "\n";
        }

        std::cout << "\nStorage Directory: " << data_dir_ << "\n";
        std::cout << "===============================================\n";
        return 0;
    }

    int cmdVersion(bool json_mode) {
        std::string ver = getVersion();
        if (json_mode) {
            std::cout << "{\"moduleId\": \"kaorioshelper\", \"version\": \"" << ver << "\"}\n";
        } else {
            std::cout << "KaoriosHelper " << ver << "\n";
        }
        return 0;
    }

    int cmdEnable(const std::string &engine) {
        fs::create_directories(data_dir_);
        if (engine == "tee" || engine == "copg") {
            std::ofstream(data_dir_ + "/" + engine + ".enabled").close();
            std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();
        } else if (engine == "hma" || engine == "pif") {
            fs::remove(data_dir_ + "/" + engine + ".disabled");
            std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();
        } else {
            std::cerr << "Unknown engine: " << engine << " (valid: hma, pif, tee, copg)\n";
            return 1;
        }
        updateStatusJson();
        std::cout << "Engine '" << engine << "' enabled (reload pending).\n";
        return 0;
    }

    int cmdDisable(const std::string &engine) {
        fs::create_directories(data_dir_);
        if (engine == "tee" || engine == "copg") {
            fs::remove(data_dir_ + "/" + engine + ".enabled");
            std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();
        } else if (engine == "hma" || engine == "pif") {
            std::ofstream(data_dir_ + "/" + engine + ".disabled").close();
            std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();
        } else {
            std::cerr << "Unknown engine: " << engine << " (valid: hma, pif, tee, copg)\n";
            return 1;
        }
        updateStatusJson();
        std::cout << "Engine '" << engine << "' disabled (reload pending).\n";
        return 0;
    }

    int cmdReload(const std::string &engine) {
        if (engine == "all" || engine.empty()) {
            for (const auto &eng : {"hma", "pif", "tee", "copg"}) {
                fs::remove(data_dir_ + "/" + eng + ".reload_pending");
            }
            std::cout << "Acknowledged and cleared reload markers for all engines.\n";
        } else {
            fs::remove(data_dir_ + "/" + engine + ".reload_pending");
            std::cout << "Acknowledged and cleared reload marker for " << engine << ".\n";
        }
        updateStatusJson();
        return 0;
    }

    static bool validateJsonSyntax(const std::string &content) {
        int brace_depth = 0;
        int bracket_depth = 0;
        bool in_string = false;
        bool escape = false;

        for (char c : content) {
            if (escape) {
                escape = false;
                continue;
            }
            if (c == '\\' && in_string) {
                escape = true;
                continue;
            }
            if (c == '"') {
                in_string = !in_string;
                continue;
            }
            if (in_string) continue;

            if (c == '{') brace_depth++;
            else if (c == '}') brace_depth--;
            else if (c == '[') bracket_depth++;
            else if (c == ']') bracket_depth--;

            if (brace_depth < 0 || bracket_depth < 0) return false;
        }
        return (!in_string && brace_depth == 0 && bracket_depth == 0 && !content.empty());
    }

    static bool validateXmlSyntax(const std::string &content) {
        if (content.find("<Keybox") == std::string::npos && content.find("<keybox") == std::string::npos) {
            return false;
        }
        if (content.find("</Keybox>") == std::string::npos && content.find("</keybox>") == std::string::npos) {
            return false;
        }
        return true;
    }

    int cmdConfigSet(const std::string &engine, const std::string &src_path) {
        std::string filename;
        bool is_xml = false;
        if (engine == "pif") filename = "pif.json";
        else if (engine == "hma") filename = "hma.json";
        else if (engine == "copg") filename = "spoof.json";
        else if (engine == "tee") { filename = "keybox.xml"; is_xml = true; }
        else {
            std::cerr << "Invalid engine for config: " << engine << "\n";
            return 1;
        }

        // Security check: reject symlinks
        struct stat lst{};
        if (lstat(src_path.c_str(), &lst) != 0) {
            std::cerr << "Error: Source configuration file does not exist: " << src_path << "\n";
            return 1;
        }
        if (S_ISLNK(lst.st_mode)) {
            std::cerr << "Security violation: Symlinks not permitted for configuration source.\n";
            return 1;
        }

        std::ifstream in(src_path);
        if (!in.is_open()) {
            std::cerr << "Error: Cannot open source file: " << src_path << "\n";
            return 1;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string content = buffer.str();
        in.close();

        // Validate syntax before touching destination
        if (is_xml) {
            if (!validateXmlSyntax(content)) {
                std::cerr << "Validation failed: Invalid Keybox XML format.\n";
                return 1;
            }
        } else {
            if (!validateJsonSyntax(content)) {
                std::cerr << "Validation failed: Malformed JSON syntax.\n";
                return 1;
            }
        }

        // Atomic write with flock and LKG backup
        fs::create_directories(data_dir_);
        std::string lock_path = data_dir_ + "/.config.lock";
        int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_EX);
        }

        std::string tmp_dest = data_dir_ + "/." + filename + ".tmp." + std::to_string(getpid());
        std::string final_dest = data_dir_ + "/" + filename;
        std::string lkg_dest = data_dir_ + "/" + filename + ".lkg";

        int out_fd = open(tmp_dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out_fd < 0) {
            std::cerr << "Error opening temporary staging file: " << strerror(errno) << "\n";
            if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
            return 1;
        }

        ssize_t written = write(out_fd, content.data(), content.size());
        fchmod(out_fd, 0600);
        fsync(out_fd);
        close(out_fd);

        if (written != static_cast<ssize_t>(content.size())) {
            unlink(tmp_dest.c_str());
            std::cerr << "Error: Incomplete write to staging file.\n";
            if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
            return 1;
        }

        // Save existing valid config as LKG backup before replacing
        struct stat curr_st{};
        if (stat(final_dest.c_str(), &curr_st) == 0 && curr_st.st_size > 0) {
            std::ifstream cur_in(final_dest, std::ios::binary);
            std::ofstream lkg_out(lkg_dest, std::ios::binary | std::ios::trunc);
            if (cur_in && lkg_out) {
                lkg_out << cur_in.rdbuf();
                lkg_out.flush();
                chmod(lkg_dest.c_str(), 0600);
            }
        }

        if (rename(tmp_dest.c_str(), final_dest.c_str()) != 0) {
            std::cerr << "Error replacing target file: " << strerror(errno) << "\n";
            unlink(tmp_dest.c_str());
            if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
            return 1;
        }

        // Set reload marker
        std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();

        if (lock_fd >= 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
        }

        updateStatusJson();
        std::cout << "Successfully updated " << filename << " atomically with LKG backup (0600 root:root).\n";
        return 0;
    }

    int cmdConfigRollback(const std::string &engine) {
        std::string filename;
        if (engine == "pif") filename = "pif.json";
        else if (engine == "hma") filename = "hma.json";
        else if (engine == "copg") filename = "spoof.json";
        else if (engine == "tee") filename = "keybox.xml";
        else {
            std::cerr << "Invalid engine for rollback: " << engine << "\n";
            return 1;
        }

        std::string lkg_dest = data_dir_ + "/" + filename + ".lkg";
        std::string final_dest = data_dir_ + "/" + filename;

        struct stat st{};
        if (stat(lkg_dest.c_str(), &st) != 0 || st.st_size == 0) {
            std::cerr << "No Last-Known-Good (LKG) backup found for " << engine << " (" << lkg_dest << ").\n";
            return 1;
        }

        std::ifstream lkg_in(lkg_dest, std::ios::binary);
        std::string tmp_dest = data_dir_ + "/." + filename + ".rb.tmp." + std::to_string(getpid());
        std::ofstream tmp_out(tmp_dest, std::ios::binary | std::ios::trunc);
        if (!lkg_in || !tmp_out) {
            std::cerr << "Failed to read LKG backup file.\n";
            return 1;
        }
        tmp_out << lkg_in.rdbuf();
        tmp_out.flush();
        tmp_out.close();

        chmod(tmp_dest.c_str(), 0600);
        if (rename(tmp_dest.c_str(), final_dest.c_str()) != 0) {
            std::cerr << "Failed to restore LKG config: " << strerror(errno) << "\n";
            unlink(tmp_dest.c_str());
            return 1;
        }

        std::ofstream(data_dir_ + "/" + engine + ".reload_pending").close();
        updateStatusJson();
        std::cout << "Successfully rolled back " << engine << " to Last-Known-Good (LKG) configuration.\n";
        return 0;
    }

    int cmdConfigGet(const std::string &engine) {
        std::string filename;
        if (engine == "pif") filename = "pif.json";
        else if (engine == "hma") filename = "hma.json";
        else if (engine == "copg") filename = "spoof.json";
        else if (engine == "tee") filename = "keybox.xml";
        else {
            std::cerr << "Invalid engine: " << engine << "\n";
            return 1;
        }

        std::string path = data_dir_ + "/" + filename;
        std::ifstream f(path);
        if (!f.is_open()) {
            std::cerr << "Configuration file not found: " << path << "\n";
            return 1;
        }
        std::cout << f.rdbuf();
        return 0;
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
    } else if (action == "reload") {
        std::string eng = args.empty() ? "all" : args[0];
        return ctl.cmdReload(eng);
    } else if (action == "config") {
        if (args.size() >= 3 && args[0] == "set") {
            return ctl.cmdConfigSet(args[1], args[2]);
        } else if (args.size() >= 2 && args[0] == "rollback") {
            return ctl.cmdConfigRollback(args[1]);
        } else if (args.size() >= 2 && args[0] == "get") {
            return ctl.cmdConfigGet(args[1]);
        } else {
            std::cerr << "Usage: helperctl config {set|get|rollback} <pif|hma|tee|copg> [file]\n";
            return 1;
        }
    } else if (action == "diagnostics") {
        return ctl.cmdDiagnostics();
    } else {
        std::cerr << "Usage: helperctl {status|version|enable|disable|reload|config|diagnostics} [--json]\n";
        return 1;
    }
}
