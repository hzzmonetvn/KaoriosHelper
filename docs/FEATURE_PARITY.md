# Feature Parity

Evidence checkpoint: 2026-11-14, implementation `bef592c`. This is source capability classification, not an end-to-end or hardware acceptance report. See [APP_MODULE_CONTRACT.md](APP_MODULE_CONTRACT.md) for the supported control/adapter subset.

## Classification

- `IMPLEMENTED`: concrete source path exists; does not imply device verification.
- `PARTIAL`: implementation exists with material contract or coverage gaps.
- `STUB`: placeholder or simulated behavior.
- `MISSING`: required implementation not established.
- `BLOCKED`: a prerequisite is absent.
- `DEVICE_UNVERIFIED`: Android behavior has not been demonstrated on hardware.

| Capability | Status | Evidence / boundary |
|---|---|---|
| Zygisk entry, companion registration, process dispatch | IMPLEMENTED | `zygisk/src/{entry,dispatcher}.cpp`; loader compatibility remains DEVICE_UNVERIFIED |
| Per-engine callback exception isolation | IMPLEMENTED | Dispatcher guards added in `ca95c37`; not a guarantee against native faults or bootloops |
| HMA privileged config snapshot and DEX loader | IMPLEMENTED / DEVICE_UNVERIFIED | `engines/hma/hma_engine.cpp`, `dex/`; `ZygoteEntry` & `EntryPoint` share full `init(String)`/`getHookState()` contract, RFC 8259 `SimpleJson` parser and deferred `system_server` bootstrap |
| HMA remote-client package filtering | PARTIAL / DEVICE_UNVERIFIED | `PackageManagerHook.java` wraps `Computer` (`mLiveComputer`/`mComputer`), `AppsFilterSnapshot` (`this$0`), `IPackageManager`, `ParceledListSlice`, `getInstallSourceInfo`, `applyPresets`/`ignoredPackagesForPresets`, and `extraOppositeAppList`; concrete non-interface `AppsFilter`/`Computer` classes remain unhooked without ART method replacement |
| HMA SettingsProvider interception | PARTIAL / DEVICE_UNVERIFIED | `SettingsProviderHook.java` wraps in-process `IContentProvider` for `adb_enabled`, `development_settings_enabled`, `mock_location`, and `settingsTemplates` with system UID exemptions; hardware verification pending |
| PIF JNI Build assignments and native property hooks | IMPLEMENTED / DEVICE_UNVERIFIED | `engines/pif/pif_engine.cpp`; handles PlayIntegrityFork v18 `VERSION.*`, `*api_level`, string flags, `pif.prop` fallback/migration; actual callbacks and resulting values DEVICE_UNVERIFIED |
| PIF custom provider / effective LKG recovery | IMPLEMENTED / DEVICE_UNVERIFIED | In-memory LKG (`last_valid_props_` + snapshot) populated and restored on error with TEE `spoofProvider=0` enforcement; Java `CustomProvider`/`CustomKeyStoreSpi` (`es.chiteroman.playintegrityfix.EntryPoint`) bundled in `classes.dex` and installed pre/post-specialize when `spoofProvider=1` and TEE inactive; device verification pending |
| TEE structural keybox inspection | PARTIAL | `engines/tee/tee_engine.cpp`; not cryptographic key/certificate validation |
| TEE attestation runtime | BLOCKED | Runtime activity remains false; supervisor/interceptor/certgen dependency chain absent |
| COPG profile selection, JNI, PLT and late-load paths | IMPLEMENTED / DEVICE_UNVERIFIED | `engines/copg/copg_engine.cpp`; `PACKAGES_*_DEVICE`, `cpu_only_packages`/`blacklist`, actual JNI assignment reporting and Bionic `libc.so` ShadowHook coverage for late-loaded `dlopen` libraries |
| COPG namespace CPU mount & companion IPC | IMPLEMENTED / DEVICE_UNVERIFIED | `include/companion_{ipc,server}.hpp`; `exemptFd` pre-specialize session retention, bounded multi-command framing, live UID/namespace checks and trusted `cpuinfo_spoof` validation |
| Native config set/get/rollback/validate/status | IMPLEMENTED / DEVICE_UNVERIFIED | Native ingress, validated LKG, per-engine request generations and `--redacted`/`summary` read (`redactedConfigRead`); text mutation errors remain |
| Kaorios JSON storage | IMPLEMENTED | Allowlisted scalar Global mapping under `settingsGlobal`; not a Kaorios ROM runtime backend |
| Settings.Global config synchronization | IMPLEMENTED SUBSET / DEVICE_UNVERIFIED | On-demand fixed-argv reads and source-derived adapters; unsupported HMA/COPG semantics fail before publication; no write-back/watcher |
| Recoverable native transaction and rollback | IMPLEMENTED | Descriptor-relative redo journal and cooperating reader lock; arbitrary-reader atomic multi-file snapshots remain unsupported |
| Legacy migration / complete upstream schema migration | PARTIAL | Explicit exact-byte legacy migration/native metadata (`COPG.json`, `pif.prop` -> `pif.json`) and canonical copy implemented; not a general schema-version migration engine |
| Runtime generation/snapshot linkage | IMPLEMENTED / DEVICE_UNVERIFIED | HMA/PIF/COPG bind ACK v2 to actual consumer snapshot, request, boot and live process; Android transport/lifecycle and full runtime remain PARTIAL |
| Python and shell runtime parity | PARTIAL | Python status/runtime delegate to native; Action/boot scripts use native commands; legacy Python config persistence remains a separate domain |
| Global boot mutations | IMPLEMENTED | Removal in `ca95c37` follows requirement 8; no compatibility exception permits global property/settings rewrites |
| Android dual-ABI build/package definitions | IMPLEMENTED | Scripts and CI define ARM64/ARM32, API 31+ and DEX; artifact and loader success require separate evidence |
| Upstream identity automation | PARTIAL | Git objects verified; source integration, transitive dependency/license closure remain separate |
| Device acceptance | DEVICE_UNVERIFIED | No hardware results for Magisk, KernelSU or APatch |

At `bef592c`, local discovery passed 135 Python tests with one artifact-dependent skip; native build and CTest 1/1 passed. CI `43583721973` passed host and Android jobs, with all 135 artifact-integration tests running in the Android job. Only diagnostic artifact `14109666874` was uploaded; dependency/license acceptance and every hardware gate remain open. Counts are run-specific, not coverage percentages. See `DEVICE_TEST_RESULTS.md`.
