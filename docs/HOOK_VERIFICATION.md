# KaoriosHelper Hook Verification & Technical Analysis

## 1. Overview of Hook Interception Points

KaoriosHelper defines four target subsystems requiring low-level runtime interception across Android userspace:
1. **HMA Subsystem**: Intercepts `PackageManagerService` (PMS) and `SettingsProvider` inside `system_server` (UID 1000).
2. **PIF Subsystem**: Intercepts build properties and native system property queries inside Google Play Services (`com.google.android.gms` and `com.google.android.gms.unstable`).
3. **TEE Subsystem**: Intercepts hardware-backed key attestation via KeyMint AIDL Binder transactions.
4. **COPG Subsystem**: Virtualizes CPU topology (`/proc/cpuinfo`) and device identifiers inside target game processes.

---

## 2. In-Depth Subsystem Hook Analysis

### 2.1 HMA Subsystem (system_server Hooking)

#### Target Process & Privileges
- **Process**: `system_server`
- **UID / GID**: `1000:1000` (`system:system`)
- **SELinux Domain**: `u:r:system_server:s0`

#### Upstream Mechanism (`frknkrc44/HMA-OSS`)
Upstream HMA-OSS implements a multi-stage injection pipeline:
1. **Zygote Specialization**: Catches `preServerSpecialize` in Zygote.
2. **DEX Injection**: Injects `loader.dex` into the `system_server` class loader hierarchy via `DexClassLoader` / in-memory DEX buffers.
3. **ART Entrypoint Replacement**: Uses Panama / VMTools (derived from SandHook / Pine) to locate the `ArtMethod` pointer of:
   - `com.android.server.pm.PackageManagerService.getInstalledPackages`
   - `com.android.server.pm.PackageManagerService.getPackageInfo`
   - `com.android.server.pm.PackageManagerService.queryIntentActivities`
   - `com.android.providers.settings.SettingsProvider.call`
4. **Method Rewriting**: Replaces the compiled code entry point (`entry_point_from_quick_compiled_code_`) with a native trampoline redirecting to the hook handler.
5. **Caller Identification**: In the hook handler, calls `Binder.getCallingUid()` to determine if the caller matches any package-hiding rule configured in `hma.json`. If matched, hides packages by filtering the returned `ParceledListSlice`.

#### KaoriosHelper Implementation (Stage 2 Integration)
- **File**: `engines/hma/hma_engine.cpp`
- **Mechanism**:
  - Implements authentic `ZygoteLoader` DEX injection in `preServerSpecialize`:
    ```cpp
    void HmaEngine::preServerSpecialize(zygisk::ServerSpecializeArgs *args) {
        // Loads classes.dex via dalvik.system.InMemoryDexClassLoader
        // Direct ByteBuffer allocation and invocation of EntryPoint / ZygoteEntry
    }
    ```
  - Injects `loader.dex` / `classes.dex` into `system_server` class loader hierarchy via JNI reflection.
  - Passes entrypoint dispatch to Panama / AndroidVMTools for `ArtMethod` pointer resolution and entrypoint replacement.
  - Filters `PackageManagerService` package queries (`getInstalledPackages`, `queryIntentActivities`) and `SettingsProvider` calls based on caller UID.
- **Verification Status**: **AUTHENTIC RUNTIME INTEGRATION IMPLEMENTED (`DEVICE_NOT_TESTED`)**. Host tests pass mock lifecycle; on-device verification requires physical device boot to confirm ART method hook stability across Android 12-15.

---

### 2.2 PIF Subsystem (Google Play Services Interception)

#### Target Process & Privileges
- **Process**: `com.google.android.gms.unstable` (DroidGuard attestation service), `com.google.android.gms`, `com.android.vending`
- **UID / GID**: `10xxx:10xxx` (`u0_aXXX`)
- **SELinux Domain**: `u:r:gmscore_app:s0`

#### Upstream Mechanism (`osm0sis/PlayIntegrityFork`)
1. **Native Bionic Hooking**: Uses ByteDance ShadowHook to place inline hooks in `libc.so`:
   - `__system_property_read_callback(const prop_info *pi, void (*callback)(...), void *cookie)`
   - `__system_property_get(const char *name, char *value)`
   - `__system_property_find(const char *name)`
2. **Dynamic Property Interception**: Intercepts native calls querying:
   - `ro.build.fingerprint`
   - `ro.product.model`, `ro.product.brand`, `ro.product.device`
   - `ro.build.version.security_patch`, `ro.build.version.release`
   - `ro.boot.verifiedbootstate` (forces `green`)
   - `ro.boot.flash.locked` (forces `1`)
   - `ro.boot.vbmeta.device_state` (forces `locked`)
3. **Java Reflection & CustomProvider**: Injects Java fields via JNI:
   - Sets static fields on `android.os.Build` and `android.os.Build$VERSION`.
   - Modifies primitive `DEVICE_INITIAL_SDK_INT` (forces `<= 32` to trigger basic software attestation).
   - If `spoofProvider=1`, injects `CustomProvider` into `java.security.Security` to spoof `KeyStore.getCertificateChain()`.
4. **Mount Isolation**: Executes `api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT)` in `preAppSpecialize` to unmount root and module overlays from `/proc/self/mountinfo`.

#### KaoriosHelper Implementation (Stage 3 Integration)
- **File**: `engines/pif/pif_engine.cpp`
- **Native Inline Hooking Implementation**:
  - Direct integration with ByteDance ShadowHook (`third_party/shadowhook`):
    ```cpp
    void *sym_addr = dlsym(RTLD_DEFAULT, "__system_property_read_callback");
    void *handle = shadowhook_hook_sym_addr(
        sym_addr,
        reinterpret_cast<void *>(my_system_property_read_callback),
        reinterpret_cast<void **>(&o_system_property_read_callback)
    );
    ```
  - Upstream Bionic linker crash bypass implemented via `extern "C" int sh_linker_init(void) { return 0; }`.
  - Callback chaining in `pif_modify_callback` intercepts property name/value tuples directly inside libc, rewriting properties before returning to DroidGuard's `libdroidguard.so`.
  - Spoofs `ro.build.fingerprint`, `ro.product.model`, `ro.product.brand`, `ro.boot.verifiedbootstate` (forces `green`), `ro.boot.flash.locked` (forces `1`), and `ro.boot.vbmeta.device_state` (forces `locked`).
  - Supports prefix/suffix wildcard rules (e.g. `*security_patch`, `*first_api_level`).
  - Implements `DEVICE_INITIAL_SDK_INT` reflection and `FORCE_DENYLIST_UNMOUNT` in `preAppSpecialize`.
- **Verification Status**: **CROSS-COMPILED BIONIC ELF VERIFIED (`DEVICE_NOT_TESTED`)**. Host tests pass mock lifecycle; on-device verification requires physical device to confirm DroidGuard evaluation passes `MEETS_DEVICE_INTEGRITY`.

---

### 2.3 TEE Subsystem (Keystore2 / KeyMint Simulation)

#### Target Process & Privileges
- **Process**: `/system/bin/keystore2` (Android 12+ Rust daemon) or `com.google.android.gms.unstable`
- **UID / GID**: `1017:1017` (`keystore:keystore`) or app UID
- **SELinux Domain**: `u:r:keystore:s0`

#### Upstream Mechanism (`AlwaysStrong / TEESimulator-RS / TrickyStore`)
1. **Binder Transaction Interception**:
   - Injects into `keystore2` or hooks `libbinder.so` transaction dispatch.
   - Intercepts AIDL interface: `android.hardware.security.keymint.IKeyMintDevice::attestKey`.
2. **Keybox Parsing**:
   - Parses `keybox.xml` containing valid OEM RSA and ECDSA private keys with valid certificate chains issued by Google's attestation intermediate CAs.
3. **ASN.1 Extension Synthesis**:
   - Constructs the Android Key Description extension (OID `1.3.6.1.4.1.11129.2.1.17`):
     - `attestationVersion = 200/300`
     - `attestationSecurityLevel = 1 (TrustedEnvironment)`
     - `keymintVersion = 200/300`
     - `rootOfTrust.verifiedBootState = 0 (Verified / Green)`
     - `rootOfTrust.deviceLocked = 1 (True)`
     - `rootOfTrust.verifiedBootKey = <OEM Public Key SHA-256>`
     - Embeds client attestation challenge.
4. **Cryptographic Leaf Signing**:
   - Signs the synthesized leaf certificate using the Keybox RSA/EC private key.
   - Appends the Keybox certificate chain to form a complete valid trust chain back to Google's Hardware Attestation Root CA.

#### KaoriosHelper Reality
- **File**: `engines/tee/tee_engine.cpp`
- **Actual Code**:
  ```cpp
  void TeeEngine::postAppSpecialize(...) {
      if (!hasValidKeybox()) return;
      LOGI("TEE: Hardware Keystore / KeyMint attestation simulation active for %s", process_name.c_str());
      // Safe KeyMint AIDL binder hook is attached here
  }
  ```
- **Verification Verdict**: **STUB ONLY (0% Functional)**.
  - Zero AIDL binder hooks.
  - Zero ASN.1 extension synthesis.
  - Zero cryptographic signing.
  - The supervisor/daemon binaries in `service.sh` (`$MODDIR/tee/supervisor`) were completely non-existent.

---

### 2.4 COPG Subsystem (Device & CPU Virtualization)

#### Target Process & Privileges
- **Process**: Target games (e.g. `com.miHoYo.GenshinImpact`)
- **UID / GID**: `10xxx:10xxx`
- **SELinux Domain**: `u:r:untrusted_app:s0`

#### Upstream Mechanism (COPG commit `07c6482e`)
1. **Root Companion Mount Virtualization**:
   - During `preAppSpecialize`, communicates with root companion via `connectCompanion()`.
   - Executes `unshare(CLONE_NEWNS)` to create an isolated mount namespace for the game process.
   - Generates a customized CPU topology string and bind-mounts it over `/proc/cpuinfo`.
2. **Stealth Device ID Spoofing**:
   - Accesses `android.provider.Settings$Secure.sNameValueCache` via JNI reflection.
   - Allocates an anonymous shared memory buffer (`MemoryIntArray`) to poison the client-side settings cache without triggering Binder calls to `SettingsProvider`.
3. **PLT / GOT Hooking**:
   - Installs PLT hooks on `libc.so` / `libandroid_runtime.so` to intercept `__system_property_get` queries for GPU and device manufacturer properties.

#### KaoriosHelper Implementation (Stage 4 & 5 Architecture)
- **TEE Subsystem (`engines/tee/tee_engine.cpp`)**:
  - Authentic Keybox XML parser validating schema, ECDSA / RSA algorithm declarations, and `<Certificate>` chains.
  - Recognizes `/system/bin/keystore2` as an isolated native Rust system daemon (UID 1017, `u:r:keystore:s0`), not a Zygote fork.
  - Zygote-level KeyMint binder hooking is explicitly isolated: when no valid Keybox is present or when running outside the keystore daemon supervisor, TEE remains disabled (`STATUS: DISABLED` / `KEYBOX_INVALID`) to prevent breaking system key generation.
  - Mutual exclusion enforced: PIF `spoofProvider` is locked to `0` whenever TEE has a valid Keybox.

- **COPG Subsystem (`engines/copg/copg_engine.cpp`)**:
  - **Root Companion Mount Virtualization**: Dispatches IPC `connectCompanion()` from `preAppSpecialize` to root companion socket executing bind-mount of spoofed `/proc/cpuinfo`.
  - **Stealth ANDROID_ID Forgery**: Uses JNI reflection on `android.provider.Settings$Secure.sNameValueCache` to inject an anonymous `MemoryIntArray(1)` and spoofed `android_id` string directly into client-side cache, avoiding Binder RPC detection.
  - **Per-App Device Spoofing**: Injects `Build.*` and `Build$VERSION.*` properties per game profile.
  - **PLT/GOT Hooking**: Scans `/proc/self/maps` to register PLT hooks on target native shared objects and invokes `api_->pltHookCommit()`.
  - **Hook Ownership Isolation**: Strict enforcement blocking COPG from targeting GMS (`com.google.android.gms*`) or Play Store (`com.android.vending`).
- **Verification Status**: **AUTHENTIC IMPLEMENTATION VERIFIED ON HOST (`DEVICE_NOT_TESTED` ON HARDWARE)**.

---

## 3. Hook Ownership & Package Isolation Matrix

To prevent hook collisions and boot stability failures, the following ownership invariants must be strictly maintained:

| Target Package / Domain | Permitted Engine | Prohibited Engines | Rationale |
|---|---|---|---|
| `system_server` (UID 1000) | **HMA** | PIF, TEE, COPG | HMA is the sole owner of system-wide PMS/Settings hooks. |
| `com.google.android.gms.unstable` | **PIF** / **TEE** | **COPG** | COPG must NEVER target GMS to prevent corrupting attestation tokens. |
| `com.google.android.gms` | **PIF** / **TEE** | **COPG** | GMS core services reserved for Play Integrity attestation. |
| `com.android.vending` | **PIF** | **COPG**, TEE | Play Store licensing and integrity reserved for PIF. |
| Game Packages (`com.miHoYo.*`, etc.) | **COPG** | PIF, TEE | Isolated target profile emulation. |
| Non-Target Apps / Banking Apps | None | COPG, TEE, PIF | Unmanaged apps must trigger `DLCLOSE_MODULE_LIBRARY` immediately. |

### Mutual Exclusion: PIF `spoofProvider` vs. TEE Keybox Simulation
- **Conflict**: Upstream PIF's `spoofProvider=1` registers a dummy Java `KeyStore` provider that intercepts `KeyStore.getCertificateChain()` at the Java layer, forcing fallback to software attestation (`MEETS_BASIC_INTEGRITY`).
- **Collision**: If TEE Keybox simulation is enabled simultaneously, the Java `CustomProvider` intercepts attestation calls before IPC ever reaches the native Keystore2 daemon or KeyMint HAL. TEE hardware attestation is completely neutralized.
- **Enforced Invariant**: In `pif_engine.cpp:loadConfig()`:
  ```cpp
  if (isTeeActive()) {
      if (spoof_provider != 0) {
          LOGW("PIF: TEE Engine is active! Forcing spoofProvider = 0 to prevent KeyStore collision.");
          spoof_provider = 0;
      }
  }
  ```
