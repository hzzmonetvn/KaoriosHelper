# Runtime Audit

## Evidence checkpoint — 2026-11-12

Baseline `ca95c37`, with configuration/upstream working-tree changes inspected separately. Five read-only source audit reports (`/tmp/kaorios-audit-reports/0.txt` through `4.txt`) establish the initial findings, not execution results. Current changes close some findings; remaining gaps below prevent an end-to-end completion claim.

## Implemented changes and limits

- `zygisk/src/dispatcher.cpp` now isolates engine callbacks with exception boundaries and records failures (`ca95c37`). This contains C++ exceptions, not arbitrary native faults, JNI errors or all system-server failures.
- `module/post-fs-data.sh` and `module/service.sh` remove global property/settings mutation paths under requirement 8. Compatibility is not an exception allowing global mutations back into boot scripts.
- Packaging/verification hardening in `ca95c37` improves required-payload and failure handling. A pipeline definition or host pass is not Android loader proof.
- Working-tree `core/config/native_store.hpp` provides pinned-directory operations, mandatory locking, no-follow checks, exclusive temporary creation, file/directory fsync and atomic single-file rename. `helperctl.cpp` uses real JSON parsing and explicit unsupported Settings.Global synchronization.
- `kaorios.json` is persisted configuration, not an implemented Kaorios runtime backend. No native-to-ROM Settings.Global mapping is defined.

## Open runtime findings

| Area | Status | Required evidence / missing contract |
|---|---|---|
| HMA entrypoint | PARTIAL | Preferred Java wrapper lacks the native init/state interface; snapshot handoff and exception handling need contract tests |
| HMA package filtering | PARTIAL | Local PackageManager/ServiceManager proxies do not establish external Binder-client filtering |
| HMA Settings interception | MISSING | Declared ownership does not install a SettingsProvider hook |
| PIF property / Build implementation | IMPLEMENTED / DEVICE_UNVERIFIED | Real Android branches exist; host hook/JNI mocks do not measure device effects |
| PIF provider / recovery | STUB | Provider consumer and populated recovery state not established |
| TEE runtime | BLOCKED | Structural XML inspection does not supply Keystore2 interception, signing or a supervisor |
| COPG implementation | PARTIAL / DEVICE_UNVERIFIED | Post-specialization companion calls, early PLT scan and assignment reporting remain gaps |
| ACK transport | PARTIAL | Pre-specialization-only companion API is used in post callbacks; concurrent persistence/framing/identity need review |
| Runtime generation/snapshot linkage | BLOCKED | PIF/COPG emit generation 0, HMA emits 1; truthful CLI cannot converge to subsequent stored generations; unified runtime is incomplete |
| Observation/status | PARTIAL | One-shot observations and stale boot/process records do not prove current hook application |
| Persistence | PARTIAL | Per-file atomicity is not atomic publication of config, generation, LKG and pending markers together |
| Migration/snapshot | MISSING | No complete versioned schema migration or crash-recoverable multi-file snapshot implementation |
| Settings.Global sync | BLOCKED | Explicit unsupported result; no settings changed |

Zygisk entry/registration must be evaluated against the actual macros and `internal::api_table` / `module_abi` definitions in `third_party/zygisk/zygisk.hpp`, not the old pseudocode signature. Host ABI self-consistency does not establish independent loader compatibility.

## Evidence boundary

Coordinating host run on 2026-11-12 reported 37 native cases, 59 Python tests and one Python skip (baseline: 36 / 43 / one skip). This document-editing pass did not rerun them. Some baseline tests construct expected states or use no-op JNI instead of exercising production Android paths. No measured line/branch coverage or hardware behavior is asserted.

Coordinating session subsequently verified CI run `43219029337` for `ca95c37` succeeded, with Android ZIP artifact `13945066444`. Coordinating session then verified final CI `43219803240` at `3045411` succeeded for host and Android. This does not establish device acceptance. Upstream Git-object identity is verified evidence, not dependency/license closure, source-to-release correspondence or runtime parity.

Release gates remain Magisk / KernelSU / APatch, API 31+, ARM64 / ARM32, actual target-process effects, unchanged unrelated processes, reboot/reload identity, upgrade/uninstall preservation and unaffected Keystore key generation. All hardware gates are DEVICE_UNVERIFIED.
