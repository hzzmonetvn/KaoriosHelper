# Feature Matrix & Capabilities

## 1. Unified Feature Inventory

This matrix details all capabilities integrated into KaoriosHelper, identifying upstream source code, runtime ownership, target environments, and verification criteria.

| Feature Area | Specific Capability | Primary Owner | Target Process | Supported Android | Supported Root Managers | Integration Status | Verification Method |
|---|---|---|---|---|---|---|---|
| **App Hiding** | Caller-to-target app isolation | HMA | `system_server` | Android 10–17 (API 29–35+) | Magisk, KSU, APatch | **Integrated** | PMS API query test (`getInstalledPackages`) |
| **App Hiding** | Per-app hidden target lists | HMA | `system_server` | Android 10–17 (API 29–35+) | Magisk, KSU, APatch | **Integrated** | Multi-target visibility test |
| **App Hiding** | Whitelist & Blacklist modes | HMA | `system_server` | Android 10–17 (API 29–35+) | Magisk, KSU, APatch | **Integrated** | Unit tests for filter evaluation |
| **App Hiding** | System app exemptions | HMA | `system_server` | Android 10–17 (API 29–35+) | Magisk, KSU, APatch | **Integrated** | Verification that system UI/services never lose visibility |
| **App Hiding** | Activity launch protection | HMA | `system_server` | Android 10–17 (API 29–35+) | Magisk, KSU, APatch | **Integrated** | Intent launch interception check |
| **App Hiding** | HMA Templates & Presets | HMA | `system_server` | Android 10–17 (API 29–35+) | Magisk, KSU, APatch | **Integrated** | Template serialization & expansion test |
| **Installer Spoofing** | Installer source filtering | HMA | `system_server` | Android 10–17 (API 29–35+) | Magisk, KSU, APatch | **Integrated** | `getInstallSourceInfo` return value check |
| **Settings Protection** | Developer options hiding | HMA | `system_server` | Android 10–17 (API 29–35+) | Magisk, KSU, APatch | **Integrated** | `Settings.Global.getInt("development_settings_enabled")` test |
| **Settings Protection** | ADB enabled status spoofing | HMA | `system_server` | Android 10–17 (API 29–35+) | Magisk, KSU, APatch | **Integrated** | `Settings.Global.getInt("adb_enabled")` test |
| **Play Integrity** | GMS device property spoofing | PIF | `com.google.android.gms.unstable` | Android 8.0–17 (API 26–35+) | Magisk, KSU, APatch | **Integrated** | `__system_property_read_callback` hook verification |
| **Play Integrity** | GMS Build field overrides | PIF | `com.google.android.gms.unstable` | Android 8.0–17 (API 26–35+) | Magisk, KSU, APatch | **Integrated** | Reflection inspect `android.os.Build` in GMS |
| **Play Integrity** | Play Store (Vending) spoofing | PIF | `com.android.vending` | Android 8.0–17 (API 26–35+) | Magisk, KSU, APatch | **Integrated** | Play Store device certification check |
| **Play Integrity** | DenyList force unmount | PIF | GMS processes | Android 8.0–17 (API 26–35+) | Magisk, KSU, APatch | **Integrated** | Check `/proc/self/mounts` in DroidGuard |
| **Attestation** | KeyMint hardware attestation simulation | TEE | `keystore2` daemon | Android 12–17 (API 31–35+) | Magisk, KSU, APatch | **Integrated** | X.509 certificate chain extension parse |
| **Attestation** | Keybox XML parsing & validation | TEE | Helper Core / TEE | Host & Device | Independent | **Integrated** | XML schema validation without logging keys |
| **Attestation** | Target package attestation isolation | TEE | `keystore2` daemon | Android 12–17 (API 31–35+) | Magisk, KSU, APatch | **Integrated** | Target list matching verification |
| **Device Spoofing** | Per-app Build property overrides | COPG | Target user apps | Android 9.0–17 (API 28–35+) | Magisk, KSU, APatch | **Integrated** | Inspect `Build.MODEL`, `BRAND` in target game |
| **Device Spoofing** | Native property GOT hooking | COPG | Target user apps | Android 9.0–17 (API 28–35+) | Magisk, KSU, APatch | **Integrated** | Hook test on `__system_property_get` |
| **Device Spoofing** | Per-app `ANDROID_ID` spoofing | COPG | Target user apps | Android 9.0–17 (API 28–35+) | Magisk, KSU, APatch | **Integrated** | `Settings.Secure.getString("android_id")` check |
| **Device Spoofing** | CPU model / profile emulation | COPG | Target user apps | Android 9.0–17 (API 28–35+) | Magisk, KSU, APatch | **Integrated** | Read `/proc/cpuinfo` in target app namespace |
| **Device Spoofing** | Game-specific high-FPS presets | COPG | Target user apps | Android 9.0–17 (API 28–35+) | Magisk, KSU, APatch | **Integrated** | Benchmark game graphic options unlocking |
| **Device Spoofing** | Concurrent microphone access | COPG | Audio subsystem | N/A | N/A | **Deferred / Unsupported** | Native source not available in open commit 07c6482e |
| **System Safety** | Verified boot property pinning | Core / PIF | System-wide early boot | Android 9.0–17 (API 28–35+) | Magisk, KSU, APatch | **Integrated** | Inspect `ro.boot.verifiedbootstate` |
| **System Safety** | Atomic configuration persistence | Core | Local file system | Host & Device | Independent | **Integrated** | Unit tests for write locks, tmp files & crash resilience |
| **Control & IPC** | Unified CLI (`helperctl`) | Core | Root shell | Android 9.0–17 (API 28–35+) | Magisk, KSU, APatch | **Integrated** | CLI invocation and status reporting tests |
| **Control & IPC** | Kaorios Toolbox Root Mode bridge | Core | Unix socket / Storage | Android 9.0–17 (API 28–35+) | Magisk, KSU, APatch | **Integrated** | JSON protocol handshake test |

---

## 2. Unsupported / Deferred Features Ledger

In strict accordance with the development rules:
1. **Concurrent Microphone Sharing**: Advertised in closed-source COPG v7.x commercial releases. The native source code for this audio flinger patch is not present in the open-source commit `07c6482eaf83bf93697e88648491abf9053121c6` or anywhere in the public tree. Marked **Deferred / Unsupported**. No reverse-engineered or closed-source binaries will be included.
2. **COPG Proprietary WebUI**: COPG's WebUI is designed for standalone browser management. KaoriosHelper is managed directly by **Kaorios Toolbox** and the `helperctl` CLI. The WebUI is omitted to reduce module footprint and eliminate unauthenticated local web servers.
3. **Automatic Unprompted Module Removal**: Competing modules (standalone HMA, TrickyStore, PIF) are detected and reported via diagnostics, but are never deleted or modified without explicit user intervention.
