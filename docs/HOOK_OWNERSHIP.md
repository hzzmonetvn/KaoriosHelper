# Hook Ownership

Evidence checkpoint: 2026-11-12. Ownership is a design constraint, not proof a hook exists or works on a device. Android build floor is API 31; broader historical API ranges are not qualified support.

| Boundary | Intended owner | Current status | Scope / limitation |
|---|---|---|---|
| Package visibility / installer information | HMA | PARTIAL | system_server Java proxy scaffolding; remote-client service-boundary coverage unproven |
| Developer/ADB Settings reads | HMA | MISSING | No installed SettingsProvider interception established |
| GMS/Play Store property and Build paths | PIF | IMPLEMENTED / DEVICE_UNVERIFIED | Native process predicates are authoritative; do not infer exact scope from a simplified diagram |
| Key attestation runtime | TEE | BLOCKED | Reserved ownership only; active Keystore2 backend absent |
| Configured app Build/property paths | COPG | PARTIAL / DEVICE_UNVERIFIED | Native exclusion of GMS/Play Store limits overlap; late libraries and subprocess profile matching need validation |
| Configured app CPU mount | COPG | PARTIAL / DEVICE_UNVERIFIED | Companion lifecycle and confinement checks remain unresolved |
| Global boot properties / Settings writes | None | MISSING by requirement | Requirement 8 removes global mutations; compatibility is not an exception |
| Kaorios ROM Settings.Global bridge | None implemented | BLOCKED | Native-config mapping absent; sync reports unsupported |

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
