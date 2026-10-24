/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * KaoriosHelper Android On-Device Diagnostic Test Fixture
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/system_properties.h>
#include <sys/stat.h>
#include <dlfcn.h>

struct TestResult {
    std::string name;
    std::string category;
    bool passed;
    std::string expected;
    std::string actual;
    std::string details;
};

static std::vector<TestResult> g_results;

static void record(const std::string &name, const std::string &category, bool passed,
                   const std::string &expected, const std::string &actual, const std::string &details = "") {
    g_results.push_back({name, category, passed, expected, actual, details});
}

// 1. System Property Interception Test
static void test_system_properties() {
    auto test_prop = [](const char *key) {
        char val[PROP_VALUE_MAX] = {0};
        int len = __system_property_get(key, val);
        std::string s_val = (len > 0) ? std::string(val) : "<NOT_FOUND>";
        return s_val;
    };

    std::string fp = test_prop("ro.build.fingerprint");
    record("prop.ro.build.fingerprint", "PIF", !fp.empty() && fp != "<NOT_FOUND>",
           "Non-empty spoofed or OEM fingerprint", fp);

    std::string bootstate = test_prop("ro.boot.verifiedbootstate");
    record("prop.ro.boot.verifiedbootstate", "PIF", bootstate == "green",
           "green", bootstate, "Play Integrity requires green for locked bootloader illusion");

    std::string flash_locked = test_prop("ro.boot.flash.locked");
    record("prop.ro.boot.flash.locked", "PIF", flash_locked == "1",
           "1", flash_locked);

    // Test callback resolution
    typedef void (*T_PropCallback)(void *cookie, const char *name, const char *value, uint32_t serial);
    typedef void (*T_ReadCallback)(const prop_info *pi, T_PropCallback callback, void *cookie);

    auto p_read_cb = reinterpret_cast<T_ReadCallback>(dlsym(RTLD_DEFAULT, "__system_property_read_callback"));
    record("libc.__system_property_read_callback", "PIF_HOOK", p_read_cb != nullptr,
           "Symbol present in libc", p_read_cb ? "Found" : "Missing");
}

// 2. /proc/cpuinfo Mount Confinement Test
static void test_cpuinfo() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    bool openable = cpuinfo.is_open();
    std::string first_line;
    if (openable) {
        std::getline(cpuinfo, first_line);
    }
    record("proc.cpuinfo.readable", "COPG", openable, "true", openable ? "true" : "false");

    // Check if cpuinfo has spoofed marker or valid processor fields
    bool has_processor = (first_line.find("processor") != std::string::npos ||
                          first_line.find("Processor") != std::string::npos ||
                          first_line.find("Hardware") != std::string::npos);
    record("proc.cpuinfo.structure", "COPG", has_processor, "Valid cpuinfo header", first_line);
}

// 3. ANDROID_ID / Settings Secure Access Test
static void test_android_id() {
    // In pure native C++, reading android_id requires querying settings service or content provider
    FILE *pipe = popen("/system/bin/settings get secure android_id 2>/dev/null", "r");
    std::string aid;
    if (pipe) {
        char buf[128] = {0};
        if (fgets(buf, sizeof(buf), pipe)) {
            aid = buf;
            while (!aid.empty() && (aid.back() == '\n' || aid.back() == '\r')) aid.pop_back();
        }
        pclose(pipe);
    }
    bool valid_aid = !aid.empty() && aid != "null";
    record("settings.secure.android_id", "COPG", valid_aid, "16-char hex string", aid);
}

// 4. Hardware Attestation / Keystore Interception Test
static void test_keystore() {
    struct stat st_ks2{};
    bool ks2_present = (stat("/system/bin/keystore2", &st_ks2) == 0);
    record("keystore2.daemon.present", "TEE", ks2_present, "true", ks2_present ? "true" : "false",
           "Keystore2 isolated native daemon");

    // Test whether keybox.xml is deployed
    const char *kb_paths[] = {
        "/data/adb/modules/kaorioshelper/data/keybox.xml",
        "/data/adb/modules/kaorioshelper/keybox.xml",
        "/data/adb/modules/TrickyStore/keybox.xml"
    };
    bool kb_found = false;
    std::string kb_loc;
    for (const auto *p : kb_paths) {
        if (access(p, R_OK) == 0) {
            kb_found = true;
            kb_loc = p;
            break;
        }
    }
    record("tee.keybox.present", "TEE", kb_found, "Valid keybox deployed", kb_found ? kb_loc : "None");
}

// 5. Package Manager Visibility Test
static void test_package_visibility() {
    const char *sensitive_pkgs[] = {
        "com.topjohnwu.magisk",
        "io.github.a13e300.ksu",
        "me.bmax.apatch",
        "org.lsposed.manager"
    };

    for (const auto *pkg : sensitive_pkgs) {
        std::string cmd = "/system/bin/pm path " + std::string(pkg) + " 2>/dev/null";
        FILE *pipe = popen(cmd.c_str(), "r");
        std::string res;
        if (pipe) {
            char buf[256] = {0};
            if (fgets(buf, sizeof(buf), pipe)) res = buf;
            pclose(pipe);
        }
        bool hidden_or_absent = res.empty();
        record(std::string("pms.hidden.") + pkg, "HMA", hidden_or_absent, "hidden or not installed",
               hidden_or_absent ? "HIDDEN / NOT_FOUND" : "VISIBLE (" + res + ")");
    }
}

static void print_json() {
    std::cout << "{\n";
    std::cout << "  \"timestamp\": " << time(nullptr) << ",\n";
    std::cout << "  \"tests\": [\n";
    for (size_t i = 0; i < g_results.size(); ++i) {
        const auto &r = g_results[i];
        std::cout << "    {\n";
        std::cout << "      \"name\": \"" << r.name << "\",\n";
        std::cout << "      \"category\": \"" << r.category << "\",\n";
        std::cout << "      \"passed\": " << (r.passed ? "true" : "false") << ",\n";
        std::cout << "      \"expected\": \"" << r.expected << "\",\n";
        std::cout << "      \"actual\": \"" << r.actual << "\",\n";
        std::cout << "      \"details\": \"" << r.details << "\"\n";
        std::cout << "    }" << (i + 1 < g_results.size() ? "," : "") << "\n";
    }
    std::cout << "  ]\n";
    std::cout << "}\n";
}

static void print_human() {
    std::cout << "\n========================================================\n";
    std::cout << "   KaoriosHelper On-Device Runtime Diagnostic Fixture   \n";
    std::cout << "========================================================\n";
    int pass_count = 0;
    for (const auto &r : g_results) {
        std::cout << (r.passed ? "[PASS] " : "[FAIL] ")
                  << "[" << r.category << "] " << r.name << "\n"
                  << "       Expected: " << r.expected << "\n"
                  << "       Actual:   " << r.actual << "\n";
        if (!r.details.empty()) {
            std::cout << "       Details:  " << r.details << "\n";
        }
        if (r.passed) pass_count++;
    }
    std::cout << "========================================================\n";
    std::cout << "Summary: " << pass_count << " / " << g_results.size() << " tests passed\n";
    std::cout << "========================================================\n";
}

int main(int argc, char *argv[]) {
    bool json_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") {
            json_mode = true;
        }
    }

    test_system_properties();
    test_cpuinfo();
    test_android_id();
    test_keystore();
    test_package_visibility();

    if (json_mode) {
        print_json();
    } else {
        print_human();
    }
    return 0;
}
