# KaoriosHelper Runtime Reality Audit

## 1. Executive Summary & Verdict

This audit provides an adversarial, evidence-based evaluation of the **KaoriosHelper** framework (`kaorioshelper`), examining whether it functions as an authentic Android Root + Zygisk module or represents an architectural prototype with simulated/mocked engine behaviors.

### Final Verdict: Architectural Prototype with Verified Integration Gaps (Do Not Ship as Production Zygisk Root Module)

While KaoriosHelper features a well-structured C++ dispatcher, robust host-side Python testing fixtures (29 unit tests), and comprehensive unit testing harnesses (25 native tests), **the underlying engine implementations on device were largely non-functional stubs prior to hardening**. 

Specifically:
- **HMA Engine**: Contained **0%** functional hooking. No Panama/VMTools ART method replacement, no ZygoteLoader DEX injection, and no `PackageManagerService` binder interception. It was solely a log statement.
- **PIF Engine**: Applied Java reflection to `android.os.Build` String fields, but lacked native Bionic `libc.so` `__system_property_read_callback` inline hooks. Furthermore, reading `/data/adb/.../pif.json` during `postAppSpecialize` failed under real Android Linux DAC and SELinux sandbox constraints.
- **TEE Engine**: Contained **0%** functional Keystore2 or KeyMint AIDL binder interception. The supervisor and daemon binaries referenced in `service.sh` (`$MODDIR/tee/supervisor`) did not exist in the codebase.
- **COPG Engine**: Contained target package parsing, but zero `/proc/cpuinfo` mount virtualization and zero PLT/GOT property hooks. The referenced controller binary (`$MODDIR/copg/controller`) did not exist.
- **Python Runtime**: The configuration control plane (`core/*.py`) was written in Python 3. Stock Android, Magisk, KernelSU, and APatch **do not include Python 3**. These scripts were completely inexecutable on an Android device.
- **Architecture Mismatch**: The packaged ZIP contained a host Linux x86_64 ELF library placed at `module/zygisk/arm64-v8a.so`, which the Android ARM64 dynamic linker rejects immediately.

Through this hardening cycle, the Zygisk ABI registration contract was restored, configuration timing was relocated to `preAppSpecialize` (before UID drop) to resolve DAC/SELinux `EACCES`, and an independent, zero-dependency compiled native C++ binary (`helperctl`) was built and packaged to replace on-device Python dependencies.

---

## 2. In-Depth Subsystem Audits

### 2.1 Zygisk Lifecycle & IPC Dispatcher
- **Audited Components**: `third_party/zygisk/zygisk.hpp`, `zygisk/src/entry.cpp`, `zygisk/src/dispatcher.cpp`
- **Initial Implementation Status**: Broken ABI & Inverted Lifecycle Timing
- **Audit Findings**:
  1. **Macro Signature & Registration Contract Defect**:
     The initial `REGISTER_ZYGISK_MODULE` macro declared `zygisk_module_entry(zygisk::Api *api, JNIEnv *env)` and instantiated a static local instance without invoking `table->register_module()`. The official Zygisk ABI (Magisk, KernelSU, APatch) expects:
     ```cpp
     void zygisk_module_entry(zygisk::internal::module_abi *table, JNIEnv *env) {
         table->register_module(table, new clazz());
     }
     ```
     Under a real Zygisk loader, treating `module_abi*` as `Api*` resulted in immediate ABI memory corruption or SIGSEGV on virtual method calls, and Zygisk never received the module pointer.
  2. **Lifecycle Timing & DAC/MAC Traversal**:
     All configuration reading was originally scheduled in `postAppSpecialize` and `postServerSpecialize`. By that point, app processes had dropped privileges to unprivileged UIDs (`u0_a...`) and transitioned to restricted SELinux domains (`u:r:untrusted_app:s0`, `u:r:gmscore_app:s0`). Accessing `/data/adb/modules/kaorioshelper/data/` (mode `0700 root:root`) triggered immediate `EACCES` and SELinux `neverallow` violations.
  3. **Null `nice_name` Unloading Race**:
     `dispatcher.cpp` called `api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY)` if `active_app_engines_` was empty. When `args->nice_name` was null during early process fork, `getProcessName` returned an empty string, causing the dispatcher to prematurely unload the library from managed target processes.
- **Hardening Applied**:
  - Replaced `third_party/zygisk/zygisk.hpp` with the official `internal::module_abi` registration contract.
  - Relocated configuration parsing and denylist unmount calls to `preAppSpecialize` and `preServerSpecialize` while the process still runs with root capabilities (UID 0) in the Zygote mount namespace.
  - Updated `dispatcher.cpp` to fall back to extracting the package name from `args->app_data_dir` when `args->nice_name` is null.

---

### 2.2 HMA Subsystem (Hide My Applist)
- **Audited Components**: `engines/hma/hma_engine.cpp`, `engines/hma/hma_engine.hpp`
- **Upstream Comparison**: `frknkrc44/HMA-OSS` (commit `6c3d1e`)
- **Initial Implementation Status**: 0% Functional (Stub / Log-Only)
- **Audit Findings**:
  - Upstream HMA-OSS operates by injecting a compiled DEX payload (`loader.dex` / `hma.dex`) into `system_server` via ZygoteLoader, replacing `ArtMethod` entry points using Panama/VMTools, and dynamically intercepting `IPackageManager` (`getInstalledPackages`, `getPackageInfo`, `queryIntentActivities`) and `SettingsProvider`.
  - In `kaorioshelper`, `hma_engine.cpp` contained **zero** JNI hooks, zero bytecode injection, and zero method replacement.
  - In `postServerSpecialize`, it merely logged:
    ```cpp
    LOGI("HMA: In-process PMS and SettingsProvider protection active");
    ```
  - Furthermore, `loadConfig()` only checked `stat()` file existence and never parsed the JSON scope rules or templates in `hma.json`.
  - True HMA package hiding in this environment relies exclusively on external ROM framework smali patches (`patch-services-a17.py`), not the in-process engine.

---

### 2.3 PIF Subsystem (Play Integrity Fix)
- **Audited Components**: `engines/pif/pif_engine.cpp`, `engines/pif/pif_engine.hpp`
- **Upstream Comparison**: `osm0sis/PlayIntegrityFork` (commit `23ec56c`)
- **Initial Implementation Status**: Defective Java-Only Reflection
- **Audit Findings**:
  1. **Java Reflection vs. Native System Property Hooks**:
     `pif_engine.cpp` only reflected `android.os.Build` and `android.os.Build$VERSION` static String fields. It had no inline hooks on Bionic `libc.so` (`__system_property_read_callback`, `__system_property_get`).
  2. **DroidGuard Detection Mechanism**:
     Google Play Services DroidGuard executes native C++ binaries (`libdroidguard.so`) that query properties directly from Bionic libc or `/dev/__properties__`. DroidGuard compares native libc property values against Java `Build` fields; when a mismatch is detected, attestation is rejected. Java reflection alone is insufficient to pass Play Integrity attestation.
  3. **Primitive Type Truncation**:
     `Build.VERSION.DEVICE_INITIAL_SDK_INT` is an integer primitive (`I`), which `pif_engine.cpp` failed to set because it only operated on `Ljava/lang/String;`.
  4. **Denylist Unmount Omission**:
     It failed to invoke `FORCE_DENYLIST_UNMOUNT`, leaving root mounts visible in `/proc/self/mountinfo`.
- **Hardening Applied**:
  - Implemented `preAppSpecialize` config caching so `pif.json` is read before UID drop.
  - Added integer field reflection for `Build.VERSION.DEVICE_INITIAL_SDK_INT`.
  - Added automatic invocation of `api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT)` for GMS target processes.

---

### 2.4 TEE Subsystem (TrickyStore / Keystore2 Attestation Simulation)
- **Audited Components**: `engines/tee/tee_engine.cpp`, `module/service.sh`
- **Upstream Comparison**: `5ec1cff/TrickyStore`, `TEESimulator-RS`
- **Initial Implementation Status**: Complete Simulation Stub
- **Audit Findings**:
  1. **Absence of AIDL Binder Interception**:
     Line 61 of `tee_engine.cpp` was literally an empty comment:
     ```cpp
     // Safe KeyMint AIDL binder hook is attached here
     ```
     No binder driver (`/dev/binder`) transactions, no `android.hardware.security.keymint.IKeyMintDevice` interceptors, and no X.509 ASN.1 certificate synthesis (OID `1.3.6.1.4.1.11129.2.1.17`) existed.
  2. **Phantom Daemon Binaries in `service.sh`**:
     Lines 57-67 of `module/service.sh` attempted to launch:
     ```sh
     "$MODDIR/tee/supervisor" "$MODDIR/tee/daemon" "$MODDIR/tee"
     ```
     Neither the `/module/tee` directory nor the `supervisor` / `daemon` executables were present in the repository, build scripts, or packaged ZIP.
  3. **Keybox Validation Facade**:
     `hasValidKeybox()` only checked if `keybox.xml` had non-zero byte size. It did not parse RSA/EC private keys or certificate chains.

---

### 2.5 COPG Subsystem (Custom Optimization & Profile Generator)
- **Audited Components**: `engines/copg/copg_engine.cpp`, `module/service.sh`
- **Upstream Comparison**: COPG commit `07c6482e`
- **Initial Implementation Status**: Target Parser Only; Hooks Unimplemented
- **Audit Findings**:
  1. Upstream COPG implements root companion IPC to create an unshared mount namespace (`unshare(CLONE_NEWNS)`), bind-mount a spoofed `/proc/cpuinfo`, poison `Settings$Secure.sNameValueCache` via `MemoryIntArray` for stealth `ANDROID_ID` spoofing, and hook PLT/GOT property tables.
  2. In `kaorioshelper`, `copg_engine.cpp` parsed target packages from `spoof.json`, but `postAppSpecialize` only logged:
     ```cpp
     LOGI("COPG postAppSpecialize: applying game & device profile emulation for %s", process_name.c_str());
     ```
  3. `module/service.sh` referenced an unbundled binary: `"$MODDIR/copg/controller"`.
  4. The companion socket callback `kaorios::companion_handler` in `entry.cpp` was a no-op function.

---

### 2.6 The On-Device Python Runtime Problem
- **Audited Components**: Entire `core/` tree (`config_manager.py`, `runtime_manager.py`, `status_manager.py`, `hook_registry.py`, `diagnostic_logger.py`)
- **Initial Implementation Status**: Inexecutable on Android
- **Audit Findings**:
  - Android does not include Python in `/system/bin`, nor do Magisk, KernelSU, or APatch package a Python runtime.
  - The shebang `#!/usr/bin/env python3` failed immediately because `/usr/bin/env` does not exist in Android's filesystem layout.
  - Any on-device attempt to invoke `python3 core/...` yielded `exit code 127 (not found)`.
  - Consequently, the Python code existed solely as host test fixtures and could not perform on-device configuration management or status reporting.
- **Hardening Applied**:
  - Re-implemented configuration management, atomic file writes, POSIX advisory locking (`flock`), schema validation, and status reporting as a native compiled C++ binary: `cli/helperctl/helperctl.cpp`.
  - Added `helperctl` to CMake build targets and updated `scripts/package_module.py` to bundle `helperctl` into the root of the flashable ZIP with `0755` permissions.
  - Updated `module/helperctl.sh` and `module/service.sh` to delegate directly to the compiled native `helperctl` executable.

---

### 2.7 False Status Reporting Deception
- **Audited Components**: `module/service.sh`, `module/helperctl.sh`
- **Initial Implementation Status**: Fabricated Status Strings
- **Audit Findings**:
  `service.sh` previously generated `status.json` by blindly writing `"status": "APPLIED"` for HMA and PIF, and `"APPLIED"` for TEE/COPG if marker files existed, regardless of whether any hook succeeded or any process crashed.
- **Hardening Applied**:
  Updated `helperctl.cpp` and `service.sh` to output truthful, audit-accurate statuses:
  - HMA: `CONFIGURED_NOT_HOOKED`
  - PIF: `PARTIALLY_APPLIED`
  - TEE: `STUB_NO_KEYMINT_AIDL`
  - COPG: `TARGETS_CONFIGURED_STUB_HOOK`

---

## 3. Summary of Verified Bugs and Remediation Status

| ID | Issue Description | Severity | Remediation Status |
|---|---|---|---|
| **BUG-01** | `REGISTER_ZYGISK_MODULE` ABI signature mismatch | CRITICAL | **FIXED** (Official `module_abi` implemented in `zygisk.hpp`) |
| **BUG-02** | DAC / SELinux `EACCES` on `postAppSpecialize` config read | CRITICAL | **FIXED** (Config loading moved to `preAppSpecialize` at UID 0) |
| **BUG-03** | Python 3 dependency on Android target | CRITICAL | **FIXED** (Native C++ `helperctl` built and packaged) |
| **BUG-04** | Premature `DLCLOSE` when `nice_name` is null | HIGH | **FIXED** (Fallback to `app_data_dir` in `dispatcher.cpp`) |
| **BUG-05** | Host x86_64 ELF packaged as arm64-v8a.so | CRITICAL | **DOCUMENTED** (Requires cross-compilation with Android NDK) |
| **BUG-06** | PIF missing native Bionic libc system property hooks | HIGH | **PARTIAL** (Java reflection + int field fixed; ShadowHook hook pending) |
| **BUG-07** | PIF omitting `FORCE_DENYLIST_UNMOUNT` | MEDIUM | **FIXED** (Added to `PifEngine::preAppSpecialize`) |
| **BUG-08** | Non-existent TEE supervisor/daemon in `service.sh` | HIGH | **FIXED** (Phantom calls removed from `service.sh`) |
| **BUG-09** | Non-existent COPG controller in `service.sh` | HIGH | **FIXED** (Phantom calls removed from `service.sh`) |
| **BUG-10** | Fraudulent `"status": "APPLIED"` reporting in shell scripts | MEDIUM | **FIXED** (Truthful audit statuses enforced) |
