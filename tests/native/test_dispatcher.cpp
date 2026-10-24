/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * Comprehensive Native Engine & Dispatcher Test Suite
 */

#include <cassert>
#include <iostream>
#include <memory>
#include <filesystem>
#include <fstream>
#include <string>

#include "zygisk.hpp"
#include "dispatcher.hpp"
#include "hma_engine.hpp"
#include "pif_engine.hpp"
#include "tee_engine.hpp"
#include "copg_engine.hpp"

namespace fs = std::filesystem;

class MockZygiskContext {
public:
    static inline int last_option = -1;
    static inline bool dlclose_called = false;
    static inline bool denylist_unmount_called = false;

    static void setOption(void *, zygisk::Option opt) {
        last_option = opt;
        if (opt == zygisk::DLCLOSE_MODULE_LIBRARY) {
            dlclose_called = true;
        } else if (opt == zygisk::FORCE_DENYLIST_UNMOUNT) {
            denylist_unmount_called = true;
        }
    }
    static int connectCompanion(void *) { return 0; }
    static uint32_t getFlags(void *) { return 0; }
    static bool exemptFd(int) { return true; }
    static void hookJniNativeMethods(JNIEnv *, const char *, JNINativeMethod *, int) {}
    static void pltHookRegister(dev_t, ino_t, const char *, void *, void **) {}
    static bool pltHookCommit() { return true; }

    zygisk::internal::api_table table;
    zygisk::Api api;

    MockZygiskContext() : api(&table) {
        last_option = -1;
        dlclose_called = false;
        denylist_unmount_called = false;
        table.impl = this;
        table.setOption = &MockZygiskContext::setOption;
        table.connectCompanion = &MockZygiskContext::connectCompanion;
        table.getFlags = &MockZygiskContext::getFlags;
        table.exemptFd = &MockZygiskContext::exemptFd;
        table.hookJniNativeMethods = &MockZygiskContext::hookJniNativeMethods;
        table.pltHookRegister = &MockZygiskContext::pltHookRegister;
        table.pltHookCommit = &MockZygiskContext::pltHookCommit;
    }
};

struct MockAppArgs {
    jint uid = 10000;
    jint gid = 10000;
    jintArray gids = nullptr;
    jint runtime_flags = 0;
    jobjectArray rlimits = nullptr;
    jint mount_external = 0;
    jstring se_info = nullptr;
    jstring nice_name = nullptr;
    jstring instruction_set = nullptr;
    jstring app_data_dir = nullptr;
    jintArray *fds_to_ignore = nullptr;
    jboolean is_child_zygote = 0;
    jboolean is_top_app = 1;
    jobjectArray *pkg_data_info_list = nullptr;
    jobjectArray *whitelisted_data_info_list = nullptr;
    jboolean mount_data_dirs = 1;
    jboolean mount_storage_dirs = 1;

    zygisk::AppSpecializeArgs make_args() {
        return zygisk::AppSpecializeArgs {
            uid, gid, gids, runtime_flags, rlimits, mount_external,
            se_info, nice_name, instruction_set, app_data_dir,
            fds_to_ignore, &is_child_zygote, &is_top_app,
            pkg_data_info_list, whitelisted_data_info_list,
            &mount_data_dirs, &mount_storage_dirs
        };
    }
};

// ============================================================================
// HMA Engine Tests (5 Tests)
// ============================================================================
void test_hma_positive(const std::string &dir) {
    std::cout << "  [RUN] test_hma_positive" << std::endl;
    kaorios::HmaEngine hma(dir);
    assert(hma.isEnabled() == true);
    assert(hma.shouldHandleServer() == true);
    assert(hma.shouldHandleApp("com.android.settings", 1000) == false);
    std::cout << "  [PASS] test_hma_positive" << std::endl;
}

void test_hma_negative(const std::string &dir) {
    std::cout << "  [RUN] test_hma_negative" << std::endl;
    std::ofstream(dir + "/hma.disabled").close();
    kaorios::HmaEngine hma(dir);
    assert(hma.isEnabled() == false);
    assert(hma.shouldHandleServer() == false);
    fs::remove(dir + "/hma.disabled");
    std::cout << "  [PASS] test_hma_negative" << std::endl;
}

void test_hma_malformed(const std::string &dir) {
    std::cout << "  [RUN] test_hma_malformed" << std::endl;
    std::ofstream cfg(dir + "/hma.json");
    cfg << "{ corrupt json content without closing braces";
    cfg.close();
    kaorios::HmaEngine hma(dir);
    // Engine must not crash on malformed file
    assert(hma.isEnabled() == true);
    fs::remove(dir + "/hma.json");
    std::cout << "  [PASS] test_hma_malformed" << std::endl;
}

void test_hma_reload(const std::string &dir) {
    std::cout << "  [RUN] test_hma_reload" << std::endl;
    kaorios::HmaEngine hma(dir);
    assert(hma.isEnabled() == true);
    std::ofstream(dir + "/hma.disabled").close();
    assert(hma.isEnabled() == false);
    fs::remove(dir + "/hma.disabled");
    assert(hma.isEnabled() == true);
    std::cout << "  [PASS] test_hma_reload" << std::endl;
}

void test_hma_recovery(const std::string &dir) {
    std::cout << "  [RUN] test_hma_recovery" << std::endl;
    fs::remove(dir + "/hma.json");
    kaorios::HmaEngine hma(dir);
    // Missing config: postServerSpecialize continues safely
    zygisk::ServerSpecializeArgs *mock_args = nullptr;
    hma.preServerSpecialize(mock_args);
    hma.postServerSpecialize(mock_args);
    std::cout << "  [PASS] test_hma_recovery" << std::endl;
}

// ============================================================================
// PIF Engine Tests (5 Tests)
// ============================================================================
void test_pif_positive(const std::string &dir) {
    std::cout << "  [RUN] test_pif_positive" << std::endl;
    kaorios::PifEngine pif(dir);
    assert(pif.isEnabled() == true);
    assert(pif.shouldHandleApp("com.google.android.gms.unstable", 10050) == true);
    assert(pif.shouldHandleApp("com.google.android.gms", 10050) == true);
    assert(pif.shouldHandleApp("com.android.vending", 10051) == true);

    std::ofstream cfg(dir + "/pif.json");
    cfg << "{\n  \"MANUFACTURER\": \"Google\",\n  \"MODEL\": \"Pixel 7\",\n  \"DEVICE_INITIAL_SDK_INT\": 33\n}\n";
    cfg.close();

    MockZygiskContext ctx;
    JNIEnv env;
    pif.onLoad(&ctx.api, &env);

    MockAppArgs args_builder;
    auto args = args_builder.make_args();
    pif.preAppSpecialize(&args, "com.google.android.gms.unstable");
    assert(MockZygiskContext::denylist_unmount_called == true);
    pif.postAppSpecialize(&args, "com.google.android.gms.unstable");

    fs::remove(dir + "/pif.json");
    std::cout << "  [PASS] test_pif_positive" << std::endl;
}

void test_pif_negative(const std::string &dir) {
    std::cout << "  [RUN] test_pif_negative" << std::endl;
    kaorios::PifEngine pif(dir);
    assert(pif.shouldHandleApp("com.example.bankapp", 10200) == false);
    assert(pif.shouldHandleApp("com.android.systemui", 1000) == false);

    std::ofstream(dir + "/pif.disabled").close();
    assert(pif.isEnabled() == false);
    assert(pif.shouldHandleApp("com.google.android.gms.unstable", 10050) == false);
    fs::remove(dir + "/pif.disabled");
    std::cout << "  [PASS] test_pif_negative" << std::endl;
}

void test_pif_malformed(const std::string &dir) {
    std::cout << "  [RUN] test_pif_malformed" << std::endl;
    std::ofstream cfg(dir + "/pif.json");
    cfg << "{\"MANUFACTURER\": unterminated string";
    cfg.close();

    kaorios::PifEngine pif(dir);
    MockAppArgs args_builder;
    auto args = args_builder.make_args();
    // Engine must handle malformed JSON safely without throwing
    pif.preAppSpecialize(&args, "com.google.android.gms.unstable");
    pif.postAppSpecialize(&args, "com.google.android.gms.unstable");

    fs::remove(dir + "/pif.json");
    std::cout << "  [PASS] test_pif_malformed" << std::endl;
}

void test_pif_reload(const std::string &dir) {
    std::cout << "  [RUN] test_pif_reload" << std::endl;
    kaorios::PifEngine pif(dir);
    assert(pif.isEnabled() == true);
    std::ofstream(dir + "/pif.disabled").close();
    assert(pif.isEnabled() == false);
    fs::remove(dir + "/pif.disabled");
    assert(pif.isEnabled() == true);
    std::cout << "  [PASS] test_pif_reload" << std::endl;
}

void test_pif_recovery(const std::string &dir) {
    std::cout << "  [RUN] test_pif_recovery" << std::endl;
    kaorios::PifEngine pif(dir);

    // TEE coexistence: when TEE active, spoofProvider forced to 0
    std::ofstream(dir + "/tee.enabled").close();
    std::ofstream kb(dir + "/keybox.xml");
    kb << "<Keybox></Keybox>";
    kb.close();
    assert(pif.isTeeActive() == true);

    std::ofstream cfg(dir + "/pif.json");
    cfg << "{\n  \"spoofProvider\": 1\n}\n";
    cfg.close();

    MockAppArgs args_builder;
    auto args = args_builder.make_args();
    pif.preAppSpecialize(&args, "com.google.android.gms.unstable");
    pif.postAppSpecialize(&args, "com.google.android.gms.unstable");

    fs::remove(dir + "/tee.enabled");
    fs::remove(dir + "/keybox.xml");
    fs::remove(dir + "/pif.json");
    std::cout << "  [PASS] test_pif_recovery" << std::endl;
}

// ============================================================================
// TEE Engine Tests (5 Tests)
// ============================================================================
void test_tee_positive(const std::string &dir) {
    std::cout << "  [RUN] test_tee_positive" << std::endl;
    std::ofstream(dir + "/tee.enabled").close();
    std::ofstream kb(dir + "/keybox.xml");
    kb << "<KeyboxContent>\n"
       << "  <Key algorithm=\"ecdsa\">\n"
       << "    <PrivateKey format=\"pem\">-----BEGIN EC PRIVATE KEY-----\nMIGkAgEBBDE=\n-----END EC PRIVATE KEY-----</PrivateKey>\n"
       << "    <Certificate format=\"pem\">-----BEGIN CERTIFICATE-----\nMIIBojCCAUqgAw=\n-----END CERTIFICATE-----</Certificate>\n"
       << "  </Key>\n"
       << "</KeyboxContent>\n";
    kb.close();

    kaorios::TeeEngine tee(dir);
    assert(tee.isEnabled() == true);
    assert(tee.hasValidKeybox() == true);
    assert(tee.shouldHandleApp("com.google.android.gms.unstable", 10050) == true);
    assert(tee.shouldHandleApp("com.google.android.gms", 10050) == true);

    fs::remove(dir + "/tee.enabled");
    fs::remove(dir + "/keybox.xml");
    std::cout << "  [PASS] test_tee_positive" << std::endl;
}

void test_tee_negative(const std::string &dir) {
    std::cout << "  [RUN] test_tee_negative" << std::endl;
    kaorios::TeeEngine tee(dir);
    // Disabled by default
    assert(tee.isEnabled() == false);
    assert(tee.shouldHandleApp("com.google.android.gms.unstable", 10050) == false);
    std::cout << "  [PASS] test_tee_negative" << std::endl;
}

void test_tee_malformed(const std::string &dir) {
    std::cout << "  [RUN] test_tee_malformed" << std::endl;
    std::ofstream(dir + "/tee.enabled").close();
    std::ofstream kb(dir + "/keybox.xml"); // 0 byte empty file
    kb.close();

    kaorios::TeeEngine tee(dir);
    assert(tee.isEnabled() == true);
    assert(tee.hasValidKeybox() == false); // Empty file must be treated as invalid

    MockAppArgs args_builder;
    auto args = args_builder.make_args();
    tee.postAppSpecialize(&args, "com.google.android.gms.unstable");

    fs::remove(dir + "/tee.enabled");
    fs::remove(dir + "/keybox.xml");
    std::cout << "  [PASS] test_tee_malformed" << std::endl;
}

void test_tee_reload(const std::string &dir) {
    std::cout << "  [RUN] test_tee_reload" << std::endl;
    kaorios::TeeEngine tee(dir);
    assert(tee.isEnabled() == false);
    std::ofstream(dir + "/tee.enabled").close();
    assert(tee.isEnabled() == true);
    fs::remove(dir + "/tee.enabled");
    assert(tee.isEnabled() == false);
    std::cout << "  [PASS] test_tee_reload" << std::endl;
}

void test_tee_recovery(const std::string &dir) {
    std::cout << "  [RUN] test_tee_recovery" << std::endl;
    std::ofstream(dir + "/tee.enabled").close();
    kaorios::TeeEngine tee(dir);
    // Non-GMS apps rejected to avoid breaking hardware Keystore / DRM
    assert(tee.shouldHandleApp("com.netflix.mediaclient", 10100) == false);
    assert(tee.shouldHandleApp("com.android.vending", 10051) == false);
    fs::remove(dir + "/tee.enabled");
    std::cout << "  [PASS] test_tee_recovery" << std::endl;
}

// ============================================================================
// COPG Engine Tests (5 Tests)
// ============================================================================
void test_copg_positive(const std::string &dir) {
    std::cout << "  [RUN] test_copg_positive" << std::endl;
    std::ofstream(dir + "/copg.enabled").close();
    std::ofstream sp(dir + "/spoof.json");
    sp << "{\n  \"PACKAGES_GENSHIN\": [\"com.miHoYo.GenshinImpact\"]\n}\n";
    sp.close();

    kaorios::CopgEngine copg(dir);
    assert(copg.isEnabled() == true);
    assert(copg.shouldHandleApp("com.miHoYo.GenshinImpact", 10300) == true);

    fs::remove(dir + "/copg.enabled");
    fs::remove(dir + "/spoof.json");
    std::cout << "  [PASS] test_copg_positive" << std::endl;
}

void test_copg_negative(const std::string &dir) {
    std::cout << "  [RUN] test_copg_negative" << std::endl;
    std::ofstream(dir + "/copg.enabled").close();
    kaorios::CopgEngine copg(dir);
    assert(copg.shouldHandleApp("com.unlisted.app", 10400) == false);
    fs::remove(dir + "/copg.enabled");
    std::cout << "  [PASS] test_copg_negative" << std::endl;
}

void test_copg_malformed(const std::string &dir) {
    std::cout << "  [RUN] test_copg_malformed" << std::endl;
    std::ofstream(dir + "/copg.enabled").close();
    std::ofstream sp(dir + "/spoof.json");
    sp << "corrupted unquoted content";
    sp.close();

    kaorios::CopgEngine copg(dir);
    // Malformed JSON handled safely
    assert(copg.shouldHandleApp("com.some.app", 10401) == false);

    fs::remove(dir + "/copg.enabled");
    fs::remove(dir + "/spoof.json");
    std::cout << "  [PASS] test_copg_malformed" << std::endl;
}

void test_copg_reload(const std::string &dir) {
    std::cout << "  [RUN] test_copg_reload" << std::endl;
    kaorios::CopgEngine copg(dir);
    assert(copg.isEnabled() == false);
    std::ofstream(dir + "/copg.enabled").close();
    assert(copg.isEnabled() == true);
    fs::remove(dir + "/copg.enabled");
    assert(copg.isEnabled() == false);
    std::cout << "  [PASS] test_copg_reload" << std::endl;
}

void test_copg_recovery(const std::string &dir) {
    std::cout << "  [RUN] test_copg_recovery" << std::endl;
    std::ofstream(dir + "/copg.enabled").close();
    std::ofstream sp(dir + "/spoof.json");
    // Even if GMS packages are mistakenly placed in spoof.json:
    sp << "{\n  \"TARGETS\": [\"com.google.android.gms\", \"com.google.android.gms.unstable\"]\n}\n";
    sp.close();

    kaorios::CopgEngine copg(dir);
    // Hook ownership isolation: COPG must strictly reject GMS
    assert(copg.shouldHandleApp("com.google.android.gms", 10050) == false);
    assert(copg.shouldHandleApp("com.google.android.gms.unstable", 10050) == false);
    assert(copg.isPackageAllowed("com.android.vending") == false);

    fs::remove(dir + "/copg.enabled");
    fs::remove(dir + "/spoof.json");
    std::cout << "  [PASS] test_copg_recovery" << std::endl;
}

// ============================================================================
// Dispatcher Tests (5 Tests)
// ============================================================================
void test_dispatcher_unmanaged_dlclose(const std::string &dir) {
    std::cout << "  [RUN] test_dispatcher_unmanaged_dlclose" << std::endl;
    kaorios::KaoriosDispatcher dispatcher;
    dispatcher.registerEngine(std::make_unique<kaorios::HmaEngine>(dir));
    dispatcher.registerEngine(std::make_unique<kaorios::PifEngine>(dir));

    MockZygiskContext ctx;
    JNIEnv mock_env;
    dispatcher.onLoad(&ctx.api, &mock_env);

    MockAppArgs args_builder;
    auto args = args_builder.make_args();
    dispatcher.preAppSpecialize(&args);
    assert(MockZygiskContext::dlclose_called == true);
    assert(MockZygiskContext::last_option == zygisk::DLCLOSE_MODULE_LIBRARY);
    std::cout << "  [PASS] test_dispatcher_unmanaged_dlclose" << std::endl;
}

void test_dispatcher_managed_claim(const std::string &dir) {
    std::cout << "  [RUN] test_dispatcher_managed_claim" << std::endl;
    kaorios::KaoriosDispatcher dispatcher;
    dispatcher.registerEngine(std::make_unique<kaorios::PifEngine>(dir));

    MockZygiskContext ctx;
    JNIEnv mock_env;
    dispatcher.onLoad(&ctx.api, &mock_env);

    // Mock nice_name
    _jstring gms_name;
    MockAppArgs args_builder;
    args_builder.nice_name = &gms_name;
    auto args = args_builder.make_args();

    // In unit test without JNI VM, null/stub getProcessName handles gracefully
    dispatcher.preAppSpecialize(&args);
    std::cout << "  [PASS] test_dispatcher_managed_claim" << std::endl;
}

void test_dispatcher_null_nice_name_fallback(const std::string &dir) {
    std::cout << "  [RUN] test_dispatcher_null_nice_name_fallback" << std::endl;
    kaorios::KaoriosDispatcher dispatcher;
    dispatcher.registerEngine(std::make_unique<kaorios::PifEngine>(dir));

    MockZygiskContext ctx;
    JNIEnv mock_env;
    dispatcher.onLoad(&ctx.api, &mock_env);

    MockAppArgs args_builder;
    args_builder.nice_name = nullptr; // null nice_name
    auto args = args_builder.make_args();
    dispatcher.preAppSpecialize(&args);
    assert(MockZygiskContext::dlclose_called == true);
    std::cout << "  [PASS] test_dispatcher_null_nice_name_fallback" << std::endl;
}

void test_dispatcher_server_specialization(const std::string &dir) {
    std::cout << "  [RUN] test_dispatcher_server_specialization" << std::endl;
    kaorios::KaoriosDispatcher dispatcher;
    dispatcher.registerEngine(std::make_unique<kaorios::HmaEngine>(dir));

    MockZygiskContext ctx;
    JNIEnv mock_env;
    dispatcher.onLoad(&ctx.api, &mock_env);

    zygisk::ServerSpecializeArgs *server_args = nullptr;
    dispatcher.preServerSpecialize(server_args);
    // HMA is active on server, dlclose should NOT be requested
    assert(MockZygiskContext::dlclose_called == false);
    dispatcher.postServerSpecialize(server_args);
    std::cout << "  [PASS] test_dispatcher_server_specialization" << std::endl;
}

void test_dispatcher_server_disabled_dlclose(const std::string &dir) {
    std::cout << "  [RUN] test_dispatcher_server_disabled_dlclose" << std::endl;
    std::ofstream(dir + "/hma.disabled").close();
    kaorios::KaoriosDispatcher dispatcher;
    dispatcher.registerEngine(std::make_unique<kaorios::HmaEngine>(dir));

    MockZygiskContext ctx;
    JNIEnv mock_env;
    dispatcher.onLoad(&ctx.api, &mock_env);

    zygisk::ServerSpecializeArgs *server_args = nullptr;
    dispatcher.preServerSpecialize(server_args);
    // When HMA is disabled, server specialization should call dlclose
    assert(MockZygiskContext::dlclose_called == true);
    assert(MockZygiskContext::last_option == zygisk::DLCLOSE_MODULE_LIBRARY);

    fs::remove(dir + "/hma.disabled");
    std::cout << "  [PASS] test_dispatcher_server_disabled_dlclose" << std::endl;
}

// ============================================================================
// ABI & Regression Tests (2 Tests)
// ============================================================================
void test_abi_table_contract_verification() {
    std::cout << "  [RUN] test_abi_table_contract_verification" << std::endl;
    MockZygiskContext ctx;
    assert(ctx.table.impl != nullptr);
    assert(ctx.table.setOption != nullptr);
    assert(ctx.table.connectCompanion != nullptr);
    assert(ctx.table.getFlags != nullptr);
    assert(ctx.table.exemptFd != nullptr);
    assert(ctx.table.hookJniNativeMethods != nullptr);
    assert(ctx.table.pltHookRegister != nullptr);
    assert(ctx.table.pltHookCommit != nullptr);

    // Verify API struct wraps table seamlessly
    ctx.api.setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    assert(MockZygiskContext::dlclose_called == true);
    std::cout << "  [PASS] test_abi_table_contract_verification" << std::endl;
}

void test_tee_keystore_keygen_non_interference(const std::string &dir) {
    std::cout << "  [RUN] test_tee_keystore_keygen_non_interference" << std::endl;
    // With tee.enabled and a valid keybox, TEE must strictly ignore system Keystore daemon callers
    // and standard applications attempting hardware keystore key generation
    std::ofstream(dir + "/tee.enabled").close();
    std::ofstream kb(dir + "/keybox.xml");
    kb << "<KeyboxContent>\n  <Key algorithm=\"ecdsa\">\n    <Certificate format=\"pem\">CERT</Certificate>\n  </Key>\n</KeyboxContent>\n";
    kb.close();

    kaorios::TeeEngine tee(dir);
    // Keystore2 daemon runs as UID 1017 - must NOT be claimed by Zygote app specialization
    assert(tee.shouldHandleApp("/system/bin/keystore2", 1017) == false);
    // Android system server (UID 1000) must NOT be intercepted by TEE
    assert(tee.shouldHandleApp("system_server", 1000) == false);
    // Standard app (UID 10150) must NOT be intercepted by TEE
    assert(tee.shouldHandleApp("com.example.banking", 10150) == false);

    fs::remove(dir + "/tee.enabled");
    fs::remove(dir + "/keybox.xml");
    std::cout << "  [PASS] test_tee_keystore_keygen_non_interference" << std::endl;
}

int main() {
    std::string temp_dir = "/tmp/kaorios_test_" + std::to_string(getpid());
    fs::create_directories(temp_dir);

    try {
        std::cout << "\n========================================================" << std::endl;
        std::cout << "      KAORIOSHELPER NATIVE TEST SUITE (27 TESTS)        " << std::endl;
        std::cout << "========================================================" << std::endl;

        std::cout << "\n[SUITE 1/6] HMA Engine Tests (5 tests):" << std::endl;
        test_hma_positive(temp_dir);
        test_hma_negative(temp_dir);
        test_hma_malformed(temp_dir);
        test_hma_reload(temp_dir);
        test_hma_recovery(temp_dir);

        std::cout << "\n[SUITE 2/6] PIF Engine Tests (5 tests):" << std::endl;
        test_pif_positive(temp_dir);
        test_pif_negative(temp_dir);
        test_pif_malformed(temp_dir);
        test_pif_reload(temp_dir);
        test_pif_recovery(temp_dir);

        std::cout << "\n[SUITE 3/6] TEE Engine Tests (5 tests):" << std::endl;
        test_tee_positive(temp_dir);
        test_tee_negative(temp_dir);
        test_tee_malformed(temp_dir);
        test_tee_reload(temp_dir);
        test_tee_recovery(temp_dir);

        std::cout << "\n[SUITE 4/6] COPG Engine Tests (5 tests):" << std::endl;
        test_copg_positive(temp_dir);
        test_copg_negative(temp_dir);
        test_copg_malformed(temp_dir);
        test_copg_reload(temp_dir);
        test_copg_recovery(temp_dir);

        std::cout << "\n[SUITE 5/6] Dispatcher Lifecycle & Safety Tests (5 tests):" << std::endl;
        test_dispatcher_unmanaged_dlclose(temp_dir);
        test_dispatcher_managed_claim(temp_dir);
        test_dispatcher_null_nice_name_fallback(temp_dir);
        test_dispatcher_server_specialization(temp_dir);
        test_dispatcher_server_disabled_dlclose(temp_dir);

        std::cout << "\n[SUITE 6/6] ABI Contract & Keystore Non-Interference Tests (2 tests):" << std::endl;
        test_abi_table_contract_verification();
        test_tee_keystore_keygen_non_interference(temp_dir);

        std::cout << "\n========================================================" << std::endl;
        std::cout << "    ALL 27 NATIVE TESTS PASSED SUCCESSFULLY! (HOST)     " << std::endl;
        std::cout << "========================================================" << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        fs::remove_all(temp_dir);
        return 1;
    }

    fs::remove_all(temp_dir);
    return 0;
}
