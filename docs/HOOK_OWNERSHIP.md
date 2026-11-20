# Hook Ownership

Evidence checkpoint: 2026-11-18 (working tree hardening post-`bef592c`). Ownership is a design constraint, not proof a hook exists or works on a device. Android build floor is API 31; broader historical API ranges are not qualified support.

| Boundary | Intended owner | Current status | Scope / limitation |
|---|---|---|---|
| Package visibility / installer information | HMA | PARTIAL / DEVICE_UNVERIFIED | `system_server` `Computer` (`mLiveComputer`) + `AppsFilterSnapshot` (`this$0`) + `IPackageManager` proxies with `templates`, `applyPresets`, `extraOppositeAppList`, and `getInstallSourceInfo`; concrete non-interface `AppsFilter`/`Computer` classes still need ART method hooks |
| Developer/ADB Settings reads | HMA | PARTIAL / DEVICE_UNVERIFIED | `SettingsProviderHook.java` wraps in-process `IContentProvider` for `adb_enabled`, `development_settings_enabled`, `mock_location`, and `settingsTemplates` with `< 10000` UID exemptions; hardware verification pending |
| GMS/Play Store property and Build paths | PIF | IMPLEMENTED / DEVICE_UNVERIFIED | Native ShadowHook + JNI Build + `pif.prop` fallback + Java `CustomProvider` (preloaded into RAM before `FORCE_DENYLIST_UNMOUNT` when `spoofProvider=1` and TEE inactive); native process predicates remain authoritative |
| Key attestation runtime | TEE | BLOCKED | Reserved ownership only; `<AndroidAttestation>`/`Keybox` XML inspection implemented, active Keystore2 backend absent |
| Configured app Build/property paths | COPG | IMPLEMENTED / DEVICE_UNVERIFIED | Native GMS/Play Store/system exclusion (including `:subprocess`), subprocess profile fallback, partition-qualified props, and late-load Bionic `libc.so` ShadowHook coverage implemented; hardware behavior unverified |
| Configured app CPU mount | COPG | IMPLEMENTED / DEVICE_UNVERIFIED | Pre-specialize `exemptFd` companion session, `SO_PEERCRED`/live-UID/mount-namespace isolation checks, and trusted `cpuinfo_spoof` validation implemented; hardware confinement unverified |
| Global boot properties / Settings writes | None | MISSING by requirement | Requirement 8 removes global mutations; compatibility is not an exception |
| Settings.Global-to-module synchronization | Native `helperctl` adapters | IMPLEMENTED SUBSET / DEVICE_UNVERIFIED | On-demand allowlisted reads and validated merges; no Settings write-back or watcher |
| Kaorios ROM/Zygisk runtime backend | None implemented | BLOCKED | Mapped scalar storage is not a runtime consumer or ROM bridge |

## Enforcement that exists

- Native dispatcher routes app/server callbacks and requests unloading for unclaimed processes (`zygisk/src/dispatcher.cpp`). Callback exception containment was added in `ca95c37`.
- COPG rejects selected GMS/Play Store process identities in `engines/copg/copg_engine.cpp`.
- `core/registry/hook_registry.py` is a host-side policy model, not the authoritative integrated native registry. Its normalization rules must not be assumed identical to native process matching.
- Installer conflict detection warns about other modules. Warning output does not establish automatic internal-engine disablement. Other installed modules are not implicitly changed.

## Claims deliberately not made

There is no proof of universal reentrancy guards, guaranteed fail-open handling, zero collision, zero bootloop probability or framework-wide interception. Dispatcher C++ exception boundaries do not intercept all native faults or Java/framework errors. Original-result behavior needs per-hook testing, including independent external caller applications.

PIF provider/TEE coordination is incomplete: configuration or keybox presence is not active TEE runtime evidence, and the local provider implementation is absent. A reserved ownership boundary is not an operational certificate-chain implementation.

## Acceptance requirements

On API 31+ ARM64 and ARM32 with Magisk, KernelSU and APatch: record actual claimed processes, callbacks and configuration identity; test failure/re-entry paths; verify target effects and unchanged unrelated apps. Separately demonstrate stock Keystore key generation before/after enablement, upgrade and uninstall. These checks remain DEVICE_UNVERIFIED. No new bypass mechanism is introduced by this ownership documentation.
