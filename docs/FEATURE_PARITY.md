# KaoriosHelper Feature Parity Matrix

## 1. Classification Methodology

Every feature within KaoriosHelper is categorized using an evidence-based taxonomy:
- `IMPLEMENTED_AND_DEVICE_VERIFIED`: Real native/hook code present, verified on physical Android hardware.
- `IMPLEMENTED_HOST_TESTED`: Real code present, verified via unit/integration test suites on host, pending physical hardware verification.
- `PARTIALLY_IMPLEMENTED`: Functional code present, but critical paths or hooks are missing.
- `STUB_OR_SIMULATION`: Code merely logs, validates syntax, or mocks behavior without functional interception.
- `NOT_IMPLEMENTED`: Documented in architecture or specifications, but zero code exists.
- `BLOCKED`: Implementation depends on unavailable toolchain, kernel driver, or missing hardware capability.

---

## 2. Core Architecture & Dispatcher Parity

| Feature Component | Upstream Reference | KaoriosHelper Status | Classification | Audit Verification Evidence |
|---|---|---|---|---|
| **Zygisk Module Entry** | Magisk / Zygisk Next API v4 | Registered via `module_abi*` | `IMPLEMENTED_HOST_TESTED` | Fixed in `third_party/zygisk/zygisk.hpp`; 25 native tests pass. |
| **Process Routing** | Standard Zygisk Dispatcher | Maps process names & UIDs | `IMPLEMENTED_HOST_TESTED` | `dispatcher.cpp` selective dispatch to active engines. |
| **Unmanaged App Unload** | `DLCLOSE_MODULE_LIBRARY` | Unloads when no engines claim process | `IMPLEMENTED_HOST_TESTED` | Verified in `test_dispatcher_unmanaged_dlclose`. |
| **Null Process Name Fallback** | Android Zygote spec | Falls back to `args->app_data_dir` | `IMPLEMENTED_HOST_TESTED` | Fixed in `dispatcher.cpp` to prevent premature unload. |
| **Privileged Companion Socket** | Zygisk `connectCompanion` | Companion IPC registration | `PARTIALLY_IMPLEMENTED` | Socket registered; payload passing requires expanded protocol. |
| **Cross-Compiled ARM64 ELF** | Android NDK Clang | Host x86_64 ELF packaged | `BLOCKED` | Host Linux cannot cross-compile NDK ARM64 without Android NDK SDK installed. |

---

## 3. Subsystem 1: HMA (Hide My Applist)

| Feature Component | Upstream HMA-OSS | KaoriosHelper Status | Classification | Audit Verification Evidence |
|---|---|---|---|---|
| **system_server Routing** | Pre-specialization routing | Routes `system_server` (UID 1000) | `IMPLEMENTED_HOST_TESTED` | `HmaEngine::shouldHandleServer()` handles UID 1000. |
| **Early Config Loading** | Pre-specialization file load | Cached in `preServerSpecialize` | `IMPLEMENTED_HOST_TESTED` | Relocated to `preServerSpecialize` before UID drop. |
| **ZygoteLoader DEX Injection** | Injects DEX into system_server | Not implemented | `NOT_IMPLEMENTED` | No DEX file or bytecode loader in codebase. |
| **ART Method Entry Replacement** | Panama / VMTools inline hook | Not implemented | `STUB_OR_SIMULATION` | `hma_engine.cpp` contains only `LOGI` statements. |
| **IPackageManager Interception** | Hooks `getInstalledPackages` | Not implemented | `NOT_IMPLEMENTED` | Zero method hooks on `PackageManagerService`. |
| **SettingsProvider Interception** | Hooks `SettingsProvider.call` | Not implemented | `NOT_IMPLEMENTED` | Zero method hooks on `SettingsProvider`. |
| **Template & Scope Parsing** | Full JSON rule parser | Checks `stat()` file existence | `PARTIALLY_IMPLEMENTED` | Full schema in Python/helperctl; C++ parser reads file presence. |

---

## 4. Subsystem 2: PIF (Play Integrity Fix)

| Feature Component | Upstream PlayIntegrityFork | KaoriosHelper Status | Classification | Audit Verification Evidence |
|---|---|---|---|---|
| **GMS Process Identification** | Targets GMS attestation | Targets `com.google.android.gms*` | `IMPLEMENTED_HOST_TESTED` | Verified in `test_pif_positive` and `test_pif_negative`. |
| **Pre-Specialize Config Cache** | Reads config before UID drop | Reads `pif.json` in `preAppSpecialize` | `IMPLEMENTED_HOST_TESTED` | Prevents Linux DAC `0600 root:root` `EACCES` after UID drop. |
| **Denylist Root Unmount** | `FORCE_DENYLIST_UNMOUNT` | Enforced in `preAppSpecialize` | `IMPLEMENTED_HOST_TESTED` | Verified in `test_pif_positive` via MockApi. |
| **Build String Reflection** | Sets Java `Build` fields | Reflected via JNI | `IMPLEMENTED_HOST_TESTED` | String fields (`MANUFACTURER`, `MODEL`, etc.) reflected. |
| **Build Primitive Reflection** | Sets `DEVICE_INITIAL_SDK_INT` | Reflected via `SetStaticIntField` | `IMPLEMENTED_HOST_TESTED` | Added `applyIntField` in `pif_engine.cpp`. |
| **Native libc Inline Hook** | ShadowHook `__system_property_read_callback` | Not implemented | `STUB_OR_SIMULATION` | Native Bionic libc property reading unhooked. |
| **CustomProvider Ingestion** | Injects dummy Java KeyStore | Coexistence rule enforced | `PARTIALLY_IMPLEMENTED` | Forces `spoofProvider=0` when TEE is active; Java provider absent. |

---

## 5. Subsystem 3: TEE (Keystore2 Simulation)

| Feature Component | Upstream TrickyStore / TEESim | KaoriosHelper Status | Classification | Audit Verification Evidence |
|---|---|---|---|---|
| **Opt-In Enablement Marker** | File marker `tee.enabled` | Checked via `stat()` | `IMPLEMENTED_HOST_TESTED` | Verified in `test_tee_positive` and `test_tee_negative`. |
| **Keybox XML Validation** | Validates XML and key elements | Validates syntax in Python/helperctl | `IMPLEMENTED_HOST_TESTED` | Tested in `test_keybox_xml_validation_and_redaction`. |
| **GMS Target Filtering** | Targets GMS attestation service | Filters to `com.google.android.gms*` | `IMPLEMENTED_HOST_TESTED` | Verified in `test_tee_positive`. |
| **Non-GMS App Exclusion** | Rejects non-target apps | Excludes banking apps & DRM | `IMPLEMENTED_HOST_TESTED` | Verified in `test_tee_recovery`. |
| **KeyMint AIDL Binder Hook** | Hooks `IKeyMintDevice::attestKey` | Comment stub only | `STUB_OR_SIMULATION` | Line 61 of `tee_engine.cpp` is a placeholder comment. |
| **ASN.1 Extension Synthesis** | Synthesizes Key Description | Not implemented | `NOT_IMPLEMENTED` | Zero X.509 ASN.1 synthesis code in codebase. |
| **Cryptographic Leaf Signing** | Signs with Keybox private key | Not implemented | `NOT_IMPLEMENTED` | Zero OpenSSL / BoringSSL signing operations. |
| **TEE Supervisor Daemon** | Background crash supervisor | Non-existent binary | `NOT_IMPLEMENTED` | Referenced in `service.sh`, binary absent from module. |

---

## 6. Subsystem 4: COPG (Game & CPU Virtualization)

| Feature Component | Upstream COPG commit 07c6482e | KaoriosHelper Status | Classification | Audit Verification Evidence |
|---|---|---|---|---|
| **Opt-In Enablement Marker** | File marker `copg.enabled` | Checked via `stat()` | `IMPLEMENTED_HOST_TESTED` | Verified in `test_copg_positive`. |
| **Target Package Parsing** | Parses `spoof.json` | Scans all quoted package tokens | `IMPLEMENTED_HOST_TESTED` | Multi-token scanner verified in `test_copg_positive`. |
| **Hook Ownership Isolation** | Never targets GMS | Strictly rejects GMS / Vending | `IMPLEMENTED_HOST_TESTED` | Verified in `test_copg_recovery` and `test_copg_gms_target_rejected`. |
| **Mount Namespace Isolation** | `unshare(CLONE_NEWNS)` | Not implemented | `STUB_OR_SIMULATION` | Zero namespace unshare or bind mount calls. |
| **/proc/cpuinfo Virtualization** | Bind-mounts spoofed cpuinfo | Not implemented | `STUB_OR_SIMULATION` | Zero cpuinfo mount virtualization in C++. |
| **ANDROID_ID Cache Poisoning** | `sNameValueCache` via shm | Not implemented | `NOT_IMPLEMENTED` | Zero `MemoryIntArray` or Settings cache manipulation. |
| **PLT / GOT Property Hooking** | Intercepts GPU/vendor props | Not implemented | `NOT_IMPLEMENTED` | Zero PLT hooks registered in `copg_engine.cpp`. |

---

## 7. Configuration Plane & Tooling

| Feature Component | Production Requirement | KaoriosHelper Status | Classification | Audit Verification Evidence |
|---|---|---|---|---|
| **Native Control CLI** | Zero-dependency binary | Compiled C++ `helperctl` | `IMPLEMENTED_HOST_TESTED` | Built and packaged into ZIP; runs natively without Python. |
| **POSIX Advisory Locking** | `flock` on `.config.lock` | Implemented in `helperctl.cpp` | `IMPLEMENTED_HOST_TESTED` | Exclusive locking protects against concurrent write races. |
| **Atomic File Replacement** | Staging -> `fsync` -> `rename` | Implemented in `helperctl.cpp` | `IMPLEMENTED_HOST_TESTED` | Prevents partial writes on unexpected power loss. |
| **Symlink Rejection** | Rejects symlinks on source configs | Implemented via `lstat` | `IMPLEMENTED_HOST_TESTED` | Rejects `S_ISLNK` to prevent symlink traversal attacks. |
| **Last-Known-Good Rollback** | Recovers from bad configs | Implemented in Python `ConfigManager` | `PARTIALLY_IMPLEMENTED` | Fully host tested; native C++ rollback pending. |
| **Secret Credential Redaction** | Strips private keys from logs | Implemented in `diagnostic_logger.py` | `IMPLEMENTED_HOST_TESTED` | Tested in `test_xml_keybox_redaction` & `test_pem_private_key_redaction`. |
| **Truthful Status Reporting** | Reports actual hook state | Implemented in `helperctl.cpp` | `IMPLEMENTED_HOST_TESTED` | Reports `CONFIGURED_NOT_HOOKED` and `PARTIALLY_APPLIED`. |
