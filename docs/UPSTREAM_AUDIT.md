# Upstream Repository & Architecture Audit

## 1. Executive Summary

KaoriosHelper integrates four major open-source Android root ecosystems into a single, cohesive Root + Zygisk module (`kaorioshelper`):
1. **HMA-OSS** (`frknkrc44/HMA-OSS`): App visibility isolation, package manager filtering, installer source spoofing, and system settings replacement.
2. **Play Integrity Fix** (`osm0sis/PlayIntegrityFork`): Device property emulation for Google Play Services (GMS) and DroidGuard (`com.google.android.gms.unstable`).
3. **AlwaysStrong** (`Evokerr/AlwaysStrong`): Coordination layer for Play Integrity + TEE Simulator, event-driven package watching, and attestation management.
4. **COPG** (`AlirezaParsi/COPG`): Per-application device spoofing, game-specific profiles, and CPU information customization.

This audit establishes the technical baseline, verified commit pins, runtime entry points, hook mechanisms, configuration contracts, licensing boundaries, and critical risks across all four upstream repositories.

---

## 2. Upstream Audit: HMA-OSS

### 2.1 Metadata & Commit Pin
- **Repository**: `https://github.com/frknkrc44/HMA-OSS.git`
- **Default Branch**: `master`
- **Latest Relevant Stable Release / Tag**: `oss-173`
- **Pinned Commit SHA**: `1fe86ce2b4a6f21197e6e92d15e054d609b7d77c`
- **Primary License**: GNU Affero General Public License v3.0 (`AGPL-3.0-only`)
- **Key Upstream Contributors**: Furkan Karcıoğlu (`frknkrc44`), `baaad`, `HelloWorld`, `OukaroMF`, `fawyrad`, `H3XDaemon`.

### 2.2 Component Layout & Architecture
- `zygote/`: The Zygisk module injection payload injected into `system_server`.
  - `ZygoteEntry.java`: Main Java injection entry point called by `ZygoteLoader`.
  - `SystemServerHook.kt`: Hooks into Android runtime initialization (`RuntimeInit.findStaticMain` or `ZygoteInit.getOrCreateSystemServerClassLoader` on Android 12+).
  - `HMAService.kt`: Privileged service executing inside `system_server`, handling AIDL IPC (`IHMAService`), config loading, and hook orchestration.
  - `hook/`: Version-specific and subsystem-specific hooks:
    - `PmsHookTargetBase.kt`, `PmsHookTarget29.kt` through `PmsHookTarget34.kt`
    - `InstallerHookTargetBase.kt`, `InstallerHookTarget29.kt` through `InstallerHookTarget34.kt`
    - `ActivityHook.kt`, `BroadcastHook.kt`, `ContentProviderHook.kt`, `AccessibilityHook.kt`, `ImmHook.kt`, `AppDataIsolationHook.kt`.
- `common/`: Data models and utilities shared across zygote and manager:
  - `JsonConfig.kt`: Complete Kotlinx Serialization data classes defining hiding rules, templates, and presets.
  - `IHMAService.aidl`: Privileged Binder interface definition.
  - `AppPresets.kt`, `SettingsPresets.kt`: Predefined detection target lists and settings replacements.
- `app/`: Standalone manager application (Jetpack Compose UI).
- `external/AndroidVMTools`: Low-level ART memory manipulation and method entry replacement via Panama / Unsafe (`6fa2d5cbbf51c96fb81a9b5d96fdd74beac4aafe`).

### 2.3 Dependencies & Build Requirements
- **Build System**: Gradle 8.13+ with Kotlin 2.3+ and Android Gradle Plugin 8.13.2.
- **Java / JDK**: JDK 17 (Zulu or OpenJDK).
- **Submodules & Plugins**:
  - `ZygoteLoader` (`com.github.aerath-stuff.ZygoteLoader` @ `2a2160fea2ed3bdbcb84fe48114718408731d9ad`): Injects DEX into `system_server` through Zygisk companion.
  - `PanamaPort` (`aerath-stuff/PanamaPort` @ `c0d8fa431e9a747dd974b8f8c02695dc3ae38b6f`): Foreign memory access and reflection primitives on ART.
  - `dev.rikka.hidden:compat:4.4.0`: Hidden Android API access stubs.
  - `io.github.vova7878:R8Annotations:v1.0.1`: Direct R8 obfuscation directives (`@DoNotShrink`, `@DoNotObfuscate`).

### 2.4 Target Environments & Compatibility
- **Target Android Versions**: Android 10 (API 29) through Android 17 (API 35+). Dedicated PMS hook implementations exist for API 29, 30, 31, 33, and 34+.
- **Architectures**: `arm64-v8a`, `armeabi-v7a`, `x86_64`, `x86`.
- **Root Framework Compatibility**: Magisk (with native Zygisk or external Zygisk), KernelSU (with Zygisk Next), APatch (with Zygisk Next).

### 2.5 Hook Mechanisms
- **system_server Entry**: ZygoteLoader loads the compiled DEX into `system_server` before package manager services initialize.
- **Method Hooking**: Panama/VMTools (`Hooks.hook`) replaces native ART Method entry points with transformer frames, redirecting calls into `BulkHooker`.
- **PackageManager Filtering**:
  - Intercepts `IPackageManager.getInstalledPackages`, `getInstalledApplications`, `getPackageInfo`, `getApplicationInfo`, `queryIntentActivities`, and related queries.
  - Determines caller identity (`Binder.getCallingUid()`). If caller UID has hiding rules defined in `JsonConfig`, hidden packages are stripped from returned lists or rejected with `NameNotFoundException`.
- **Installer Source Filtering**:
  - Intercepts `getInstallSourceInfo` and `getInstallerPackageName`. Overwrites installer package name with trusted stores (e.g., `com.android.vending`) or clears it based on caller policy.
- **Settings Replacement**:
  - Intercepts `SettingsProvider` queries for developer settings (`development_settings_enabled`, `adb_enabled`), accessibility services, and input methods, returning spoofed stock values to target callers.

### 2.6 Configuration Schema & Manager Decoupling
- **Schema**: `JsonConfig` containing:
  - `configVersion`: Schema migration counter.
  - `detailLog`, `maxLogSize`: Diagnostic logging flags.
  - `templates`: Reusable app visibility templates (`isWhitelist`, `appList`).
  - `settingsTemplates`: Reusable settings replacement lists.
  - `scope`: Map of package name to `AppConfig` (`useWhitelist`, `excludeSystemApps`, `hideInstallationSource`, `extraAppList`, `extraOppositeAppList`).
- **Decoupling Strategy for KaoriosHelper**:
  - Upstream HMA-OSS restricts IPC to its own manager APK by verifying the caller's signing certificate via `findAndVerifyAppSignature(pms)` against hardcoded `Magic.magicNumbers`.
  - In KaoriosHelper, configuration is read directly from `/data/adb/modules/kaorioshelper/data/hma.json` by the service or pushed via privileged Root IPC / `helperctl`, eliminating the need for a separate proprietary manager APK.

### 2.7 Known Risks & Limitations
- **system_server Stability**: Any unhandled exception or deadlock inside a PMS hook crashes `system_server`, causing an immediate framework reboot or bootloop.
- **Hook Recursion**: Hook implementations must strictly avoid calling PMS methods that trigger the hooked method recursively.

---

## 3. Upstream Audit: Play Integrity Fix / PlayIntegrityFork

### 3.1 Metadata & Commit Pin
- **Repository**: `https://github.com/osm0sis/PlayIntegrityFork.git` (maintained by osm0sis; upstream descendant of chiteroman/PlayIntegrityFix)
- **Default Branch**: `master`
- **Latest Relevant Tag**: `v18` (`6d2307ff5037519982205e66bda1132d0d8cc4da`)
- **Pinned Commit SHA**: `23ec56c2440c4fd1a5e893a23ef7861d6366a828`
- **Primary License**: GNU General Public License v3.0 (`GPL-3.0-only`)
- **Key Maintainer**: `osm0sis` (following original work by `chiteroman`).

### 3.2 Component Layout & Architecture
- `app/src/main/cpp/`:
  - `main.cpp`: Native Zygisk companion (`PlayIntegrityFix` class inheriting from `zygisk::ModuleBase`).
  - `zygisk.hpp`: Zygisk C++ API header (API v4).
  - `shadowhook.h`: ByteDance ShadowHook library header for native inline hook management.
  - `json/`: Header-only `nlohmann/json` parser.
- `app/src/main/java/es/chiteroman/playintegrityfix/`:
  - `EntryPoint.java`: Java initialization invoked inside GMS target processes. Reflectively updates `android.os.Build` fields and instantiates `CustomProvider`.
  - `CustomProvider.java`: Security provider inserting `CustomKeyStoreSpi` into Java `Security.insertProviderAt(..., 1)`.
  - `CustomKeyStoreSpi.java`: KeyStore SPI wrapping hardware keystore operations to intercept certificate chain retrieval.
- `module/`: Magisk packaging scripts:
  - `post-fs-data.sh`, `service.sh`, `customize.sh`, `migrate.sh`, `autopif4.sh`, `killpi.sh`.

### 3.3 Dependencies & Build Requirements
- **Native Build**: Android NDK (r26b or newer) with CMake 3.22+.
- **Java / DEX**: JDK 17, Gradle 8+, Android SDK build-tools.
- **Third-Party Libraries**:
  - ByteDance ShadowHook (`libshadowhook.so`) under Apache-2.0.
  - `nlohmann/json` under MIT License.

### 3.4 Target Environments & Compatibility
- **Target Android Versions**: Android 8.0 (API 26) through Android 17 (API 35+).
- **Target Processes**:
  - `com.google.android.gms.unstable` (DroidGuard attestation service).
  - `com.android.vending` (Google Play Store).
  - Excluded from all normal app processes (`api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY)`).
- **Architectures**: `arm64-v8a`, `armeabi-v7a`, `x86_64`, `x86`.

### 3.5 Hook Mechanisms & GMS Injection
- **Process Filtering**: In `preAppSpecialize`, inspects `args->nice_name` and `args->app_data_dir`. If not matching GMS / DroidGuard / Vending, immediately requests `DLCLOSE_MODULE_LIBRARY`.
- **DenyList Unmount**: Calls `api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT)` in target GMS processes to isolate root mounts.
- **Native System Property Hook**:
  - ShadowHook hooks `__system_property_read_callback` in `libc.so`.
  - When DroidGuard queries system properties (e.g., `ro.build.fingerprint`, `ro.product.model`, `ro.build.version.security_patch`), the hook substitutes the values defined in `pif.json`.
- **Java Field Overrides**:
  - Loads in-memory `classes.dex` via companion socket file descriptor.
  - Calls `EntryPoint.init()`, modifying static fields in `android.os.Build` (`MANUFACTURER`, `MODEL`, `FINGERPRINT`, `BRAND`, `PRODUCT`, `DEVICE`, `ID`, `TAGS`, `TYPE`, `SECURITY_PATCH`).

### 3.6 Configuration Schema
- Primary configuration file: `pif.json` (also supports `pif.prop`).
- Expected Fields:
  ```json
  {
    "MANUFACTURER": "Google",
    "MODEL": "Pixel 7 Pro",
    "FINGERPRINT": "google/cheetah_beta/cheetah:CANARY/...",
    "BRAND": "google",
    "PRODUCT": "cheetah_beta",
    "DEVICE": "cheetah",
    "RELEASE": "14",
    "ID": "...",
    "INCREMENTAL": "...",
    "TYPE": "user",
    "TAGS": "release-keys",
    "SECURITY_PATCH": "2026-10-05",
    "DEVICE_INITIAL_SDK_INT": "33",
    "spoofBuild": 1,
    "spoofProps": 1,
    "spoofProvider": 0,
    "spoofSignature": 0
  }
  ```

### 3.7 Critical Conflict Finding
- **`spoofProvider` Collision with TEE Simulator**:
  - If `spoofProvider=1`, PIF installs `CustomProvider` into the Java runtime, intercepting `java.security.KeyStore.getCertificateChain()` and providing a dummy software keypair.
  - If a TEE Simulator (TEESimulator-RS / KeyMint interceptor) is active, PIF's `CustomProvider` intercepts the certificate chain before the native KeyMint daemon can return the genuine keybox-backed attestation extension, demoting the attestation verdict to `BASIC` or failing verification.
  - **Requirement**: `spoofProvider` must default to `0` or be disabled whenever the TEE Engine is enabled.

---

## 4. Upstream Audit: AlwaysStrong

### 4.1 Metadata & Commit Pin
- **Repository**: `https://gitlab.com/Evokerr/AlwaysStrong.git`
- **Default Branch**: `main`
- **Latest Relevant Tag**: `v1.0.5` (`72309a7c5117f28f44a1cd72b89ff1c5302208bf`)
- **Pinned Commit SHA**: `95ccd2add35a2db2721b9e5111f13917f7dbac0c`
- **Primary License**: GNU General Public License v3.0 (`GPL-3.0-only`)
- **Key Maintainer**: `Evokerr`.

### 4.2 Architecture & Reusable Concepts
AlwaysStrong is not a hook engine itself; it is an integration and build framework coordinating:
1. **Play Integrity Engine**: Selects and repackages either PlayIntegrityFork or PlayIntegrityFix.
2. **Attestation Engine**: Selects and packages `TEESimulator-RS` (default), `TrickyStoreOSS`, or `TEESimulator-JingMatrix`.
3. **Event-Driven Native Watcher** (`native/watcher`):
   - Rust daemon using Linux `inotify` on `/data/system` to detect changes to `packages.list`.
   - Automatically rebuilds `target.txt` on app installation or uninstallation without polling.
   - Detects and strips known Xposed managers (`de.robv.android.xposed.installer`, `org.lsposed.manager`, etc.) from `target.txt` to prevent hooked processes from tripping attestation checks.
4. **Resilient Network Client** (`native/asfetch`):
   - Rust static binary using `rustls` with Happy-Eyeballs (concurrent IPv4 and IPv6 resolution) to download fingerprints and certificates on devices with broken busybox TLS.
5. **Bootloader & Verified Boot Properties Management**:
   - `post-fs-data.sh` and `service.sh` set OEM-specific verified boot properties (`ro.boot.verifiedbootstate=green`, `ro.boot.vbmeta.device_state=locked`, Samsung `warranty_bit=0`, Realme `realmebootstate=green`, OnePlus `is_ever_orange=0`).

### 4.3 TEE Engine Deep Dive: TEESimulator-RS
- **Upstream Repository**: `https://github.com/Enginex0/TEESimulator-RS.git`
- **Pinned Commit**: `6d241e56d6e8146cd67ed4ac3dadc9c632969549` (Tag `v6.0.1-307`)
- **License**: GPL-3.0-only (with NOTICE acknowledging TrickyStore/TrickyStoreOSS).
- **Execution Mechanism**:
  - Two-stage supervisor/daemon process (`supervisor` + `daemon`).
  - Injects native interceptor (`libinject.so`) into `/system/bin/keystore2`.
  - Intercepts KeyMint AIDL Binder transactions (`IKeyMintDevice::generateKey`, `attestKey`).
  - When a target package (listed in `target.txt`) requests key attestation, TEESimulator synthesizes an X.509 attestation certificate extension containing genuine verified-boot properties and signs the leaf certificate using the EC/RSA private key provided in `keybox.xml`.
- **Target Android Versions**: Android 12 (API 31) through Android 17 (API 35+), ARM64-v8a.

### 4.4 Coordination Rules Derived from AlwaysStrong
1. **PIF / TEE Separation of Concerns**: PIF handles Build props and GMS system properties; TEE handles hardware-backed Keystore attestation.
2. **Boot Property Pinning**: Verified boot state must be set to `green` and vbmeta state to `locked` before `keystore2` starts to ensure `rootOfTrust` evaluation succeeds.
3. **Target Package Isolation**: Non-target packages receive standard system attestation, preventing widespread keystore disruption.

---

## 5. Upstream Audit: COPG

### 5.1 Metadata & Commit Pin
- **Repository**: `https://github.com/AlirezaParsi/COPG.git`
- **Default Branch**: `JSON`
- **Pinned Commit SHA**: `07c6482eaf83bf93697e88648491abf9053121c6`
- **Version Identifier**: COPG v5.1.1 (Historical complete open-source release)
- **Primary License**: Apache License 2.0 (`Apache-2.0`)
- **Key Author**: `AlirezaParsi` (`Aliza`).

### 5.2 Critical Native Source Code Audit & Risk Analysis
- **Missing Source in Modern Branches**:
  - In COPG branches `JSON`, `rewrite`, `bkp`, and tags `v5.4.0` through `v7.6.0`, build scripts (`build.sh`, `.github/workflows/json.yml`) reference `src/spoof_module.cpp` and `src/unified_controller.cpp`.
  - However, the `src/` directory was **deliberately deleted** from the repository after commit `07c6482eaf83bf93697e88648491abf9053121c6` when the author converted COPG into a commercial product (vendors.copg.my / COPG PRO).
  - Modern module releases ship prebuilt, closed-source binaries.
- **The Pinned Open-Source Base**:
  - Commit `07c6482eaf83bf93697e88648491abf9053121c6` contains the complete, compilable, Apache-2.0 licensed C++ source code:
    - `src/spoof_module.cpp`: Complete Zygisk module implementation.
    - `src/unified_controller.cpp`: Native userspace daemon for profile management.
    - `src/atexit.cpp`: Compatibility wrapper for `__cxa_atexit`.
    - `src/CMakeLists.txt`: NDK CMake build definition.
    - `src/include/json.hpp`: Bundled nlohmann/json (MIT).
    - `src/include/zygisk.hpp`: Bundled Zygisk API (ISC).
- **Licensing & Ethical Stance**:
  - KaoriosHelper exclusively uses and ports the open-source C++ codebase from commit `07c6482eaf83bf93697e88648491abf9053121c6`.
  - No closed-source binaries, PRO licensing mechanisms, or unverified binaries are used or redistributed.

### 5.3 Hook Mechanisms in COPG
- **GOT (Global Offset Table) Hooking**:
  - Hooks `__system_property_get` and `__system_property_read_callback` in target application ELF binaries.
  - Overrides properties (`ro.product.model`, `ro.product.brand`, etc.) for target games/apps.
- **JNI Static Field Overrides**:
  - In `postAppSpecialize`, inspects `android.os.Build` and overrides static string fields (`MODEL`, `BRAND`, `MANUFACTURER`, `DEVICE`, `PRODUCT`, `FINGERPRINT`, `SERIAL`).
- **ANDROID_ID Cache-Poisoning**:
  - Android stores `ANDROID_ID` in `Settings$Secure.sNameValueCache` backed by a `MemoryIntArray` generation tracker.
  - COPG synchronizes with `sNameValueCache`, injects a spoofed `ANDROID_ID` value, and sets the generation tracker to unchanged, avoiding binder calls to `SettingsProvider` and allowing stealth operation before `DLCLOSE`.
- **CPU Info Spoofing**:
  - Bind-mounts simulated `/proc/cpuinfo` profiles (e.g., `cpuinfo_sd8elite`, `cpuinfo_dimensity9400plus`) onto `/proc/cpuinfo` for target packages, allowing games to recognize flagship chipsets and unlock maximum frame rates.

### 5.4 Configuration Schema
- Primary configuration file: `spoof.json` (upstream `COPG.json`).
- Schema structure:
  ```json
  {
    "cpu_spoof": {
      "blacklist": [],
      "cpu_only_packages": ["com.epicgames.fortnite"]
    },
    "PACKAGES_REDMAGIC_9_PRO": [
      "com.dts.freefiremax:with_cpu",
      "com.activision.callofduty.shooter"
    ],
    "PACKAGES_REDMAGIC_9_PRO_DEVICE": {
      "BRAND": "nubia",
      "DEVICE": "REDMAGIC 9 Pro",
      "MANUFACTURER": "ZTE",
      "MODEL": "NX769J",
      "FINGERPRINT": "nubia/NX769J/NX769J:14/...",
      "PRODUCT": "NX769J",
      "ANDROID_ID": "a1b2c3d4e5f60718"
    }
  }
  ```

---

## 6. Upstream Comparison & Compatibility Summary

| Dimension | HMA-OSS | Play Integrity Fork | AlwaysStrong / TEE | COPG |
|---|---|---|---|---|
| **Primary Focus** | App visibility & isolation | GMS property spoofing | Keystore hardware attestation | Per-app device & CPU spoofing |
| **Execution Domain** | `system_server` | GMS / DroidGuard processes | `keystore2` daemon & background | Target application processes |
| **Hook Technology** | Panama / ART Entry / Reflection | ShadowHook inline + JNI | Binder interception / KeyMint | GOT hooks + JNI + /proc bind |
| **Config File** | `hma.json` | `pif.json` | `keybox.xml` + `target.txt` | `spoof.json` |
| **Reload Support** | Live AIDL / File reload | On process start / killpi | Watcher on target.txt / FS | Inotify / on process start |
| **License** | AGPL-3.0-only | GPL-3.0-only | GPL-3.0-only | Apache-2.0 |
| **Critical Collision** | None (owns PMS/Settings) | `spoofProvider` conflicts with TEE | Must disable PIF `spoofProvider` | Must never target GMS |

---

## 7. Phase 0 Acceptance Assessment

- [x] All 4 upstream repositories cloned, reviewed, and audited at the code level.
- [x] Pinned commit SHAs and tags verified for reproducible builds.
- [x] COPG source availability dilemma resolved by pinning historical open-source commit `07c6482eaf83bf93697e88648491abf9053121c6`.
- [x] Critical runtime collisions (PIF `spoofProvider` vs TEE Keystore2, COPG vs PIF GMS scope) documented with explicit architectural rules.
- [x] HMA-OSS manager decoupling strategy identified.
- [x] Upstream commit lockfile generated (`upstreams.lock.json`).
