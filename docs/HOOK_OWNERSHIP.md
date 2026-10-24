# Hook Collision Analysis & Ownership Registry

## 1. Ownership Policy Principles

In a unified Root + Zygisk framework combining multiple hook engines, uncoordinated hooks at overlapping entry points lead to:
1. Double modification (e.g. PIF and COPG both attempting to rewrite `ro.product.model` in GMS).
2. Race conditions (e.g. Keystore certificate chains intercepted by a software provider before KeyMint attestation executes).
3. Hook recursion and stack overflow in `system_server`.
4. Performance degradation in high-frequency IPC or system call paths.

To eliminate collisions, KaoriosHelper enforces strict **Single Ownership**:
- Every system entry point has exactly **one** designated primary owner engine.
- Secondary engines attempting to touch that entry point are either descheduled, scoped away by process filter, or explicitly disabled.
- In-module conflict checks prevent conflicting sub-features from running simultaneously.

---

## 2. Global Hook Ownership Matrix

| Subsystem / Entry Point | Primary Owner | Target Process Scope | Android APIs | Input / Output Semantics | Secondary Competitors | Resolution Strategy | Failure Behavior |
|---|---|---|---|---|---|---|---|
| **App Visibility (PMS)** | **HMA** | `system_server` | API 29–35+ | Filters app lists (`getInstalledPackages`, `queryIntentActivities`) | None | Exclusive HMA ownership | Return unfiltered lists; log error; do not crash |
| **Installer Source (PMS)** | **HMA** | `system_server` | API 29–35+ | Spoofs installer package name in `getInstallSourceInfo` | None | Exclusive HMA ownership | Return original installer info |
| **SettingsProvider (Dev Options, ADB)** | **HMA** | `system_server` | API 29–35+ | Overrides `development_settings_enabled`, `adb_enabled` | None | Exclusive HMA ownership | Return actual setting value |
| **GMS / DroidGuard System Properties** | **PIF** | `com.google.android.gms.unstable`, `com.android.vending` | API 26–35+ | Intercepts `__system_property_read_callback` for Play Integrity | COPG | **COPG strictly prohibited from targeting GMS packages** | Pass through unmodified system properties |
| **GMS / DroidGuard Build Fields** | **PIF** | GMS target processes only | API 26–35+ | Reflectively updates `android.os.Build.*` fields | COPG | Process boundary separation (COPG unloads in GMS) | Keep stock Build values |
| **Hardware Key Attestation (KeyMint/Keystore2)** | **TEE** | `keystore2` daemon & background attestation | API 31–35+ | Intercepts `IKeyMintDevice::attestKey`, injects keybox certificate chain | PIF (`spoofProvider`) | **PIF `spoofProvider` forced to 0** when TEE active | Fallback to stock hardware attestation or basic evaluation |
| **App-Specific Build Properties** | **COPG** | Target games & user apps (excluding GMS) | API 28–35+ | Reflectively updates `android.os.Build.*` fields in `postAppSpecialize` | PIF | PIF does not load in general user apps | App sees unmodified host Build properties |
| **App-Specific Native System Properties** | **COPG** | Target games & user apps | API 28–35+ | GOT hooks on `__system_property_get` / `read_callback` | PIF | Scoped exclusively to configured game packages in `spoof.json` | Fall back to stock system properties |
| **App-Specific ANDROID_ID** | **COPG** | Target games & user apps | API 28–35+ | Pre-seeds `Settings$Secure.sNameValueCache` generation tracker | None | Exclusive COPG ownership | Return genuine system `ANDROID_ID` |
| **CPU Information (/proc/cpuinfo)** | **COPG** | Target games & user apps | API 28–35+ | Bind-mounts `/proc/cpuinfo` profile in app mount namespace | None | Exclusive COPG ownership | Game reads host `/proc/cpuinfo` |

---

## 3. Deep Dive: Subsystem Collisions & Resolutions

### 3.1 Collision 1: Play Integrity Fix vs TEE Simulator (Keystore Certificate Chain)
- **The Conflict**:
  - Upstream PIF contains `CustomProvider.java` and `CustomKeyStoreSpi.java`, enabled when `spoofProvider=1` in `pif.json`.
  - When active, `CustomKeyStoreSpi` intercepts `java.security.KeyStore.getCertificateChain()` in DroidGuard and generates a dummy software RSA keypair, stripping hardware attestation flags to force Google Play Integrity to fall back to `MEETS_BASIC_INTEGRITY`.
  - TEE Simulator (TEESimulator-RS / TrickyStoreOSS), on the other hand, hooks the native `keystore2` / KeyMint AIDL interface to synthesize a valid hardware-like certificate chain backed by a genuine leaked Keybox certificate, aiming to achieve `MEETS_STRONG_INTEGRITY` or `MEETS_DEVICE_INTEGRITY`.
  - If PIF's `spoofProvider` is active simultaneously with TEE, PIF intercepts the certificate chain before it ever reaches the KeyMint attestation layer, neutralizing the TEE Engine completely!
- **Resolution in KaoriosHelper**:
  1. The PIF Engine in KaoriosHelper enforces `spoofProvider=0` as the default.
  2. If the TEE Engine is enabled in `RuntimeManager`, `spoofProvider` is strictly locked to `0`, preventing `CustomProvider` from being registered.
  3. PIF handles purely device properties and GMS environment flags; TEE exclusively handles the KeyStore attestation chain.

### 3.2 Collision 2: PIF vs COPG (System Properties & Process Ownership)
- **The Conflict**:
  - Both PIF and COPG modify system properties (`ro.product.model`, `ro.product.brand`, `ro.build.fingerprint`) via Zygisk.
  - If a user configures a game profile in COPG for `com.google.android.gms` or configures broad system property overrides, COPG's GOT hooks or property modifications would overwrite the carefully tuned Play Integrity fingerprint provided by PIF.
- **Resolution in KaoriosHelper**:
  1. **Strict Process Scoping**:
     - PIF **only** targets `com.google.android.gms.unstable` and `com.android.vending`. For all other processes, PIF calls `api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY)`.
     - COPG **strictly excludes** `com.google.android.gms*` and `com.android.vending` from its target list. Any entry in `spoof.json` matching GMS is automatically ignored and logged with a diagnostic warning.
  2. **Zero In-Process Overlap**: PIF and COPG never execute their hook logic in the same process instance.

### 3.3 Collision 3: HMA-OSS vs System Server Stability
- **The Conflict**:
  - HMA hooks fundamental PackageManager and SettingsProvider methods inside `system_server`.
  - If an exception is thrown inside `shouldFilterApplication` or `getInstalledPackages`, `system_server` crashes, triggering a device-wide soft reboot.
  - If an HMA hook method calls another method that re-enters the hook, an infinite recursion leads to a `StackOverflowError`.
- **Resolution in KaoriosHelper**:
  1. **Thread-Local Reentrancy Guards**: Every hook implementation uses a `ThreadLocal<Boolean>` or primitive reentrancy flag. If already inside an HMA filtering frame on the current thread, the hook immediately delegates to the original method without evaluation.
  2. **Fail-Safe Try-Catch-Delegate**: All filtering logic is wrapped in `try-catch(Throwable)`. Any unexpected error logs a structured diagnostic and returns the unmodified stock result, guaranteeing that `system_server` never crashes.

### 3.4 Collision 4: External Competing Modules
- **The Conflict**:
  - Users might install standalone versions of HMA, PlayIntegrityFix, TrickyStore, or COPG alongside KaoriosHelper.
  - Running two separate Zygisk modules hooking the same functions causes unpredictable chaining failures and crashes.
- **Resolution in KaoriosHelper**:
  1. `customize.sh`, `helperctl`, and `StatusManager` perform non-destructive conflict scanning.
  2. Checked module directories:
     - `hma_oss_zygisk`
     - `playintegrityfix` / `playintegrityfork`
     - `tricky_store` / `teesim`
     - `COPG`
  3. If a competing active module is detected:
     - KaoriosHelper warns the user and marks the affected internal engine as `CONFLICT_DETECTED` / `DISABLED`.
     - KaoriosHelper **never** automatically deletes, disables, or modifies other user-installed modules without user consent.

---

## 4. Hook Registry API Design

The centralized `HookRegistry` inside KaoriosHelper maintains the active state of all hooks:
```text
HookRegistry
├── registerHook(subsystem, ownerEngine, targetScope)
├── checkConflict(subsystem, requestingEngine) -> Result<Ok, ConflictError>
├── recordHookState(hookId, status: UNINITIALIZED | ACTIVE | FAILED | DISABLED)
└── getDiagnostics() -> HookDiagnosticsReport
```

This guarantees deterministic startup and transparent status reporting to Kaorios Toolbox.
