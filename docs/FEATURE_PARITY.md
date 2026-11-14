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
| HMA privileged config snapshot and DEX loader | PARTIAL | `engines/hma/hma_engine.cpp`; Java/native entrypoint handoff remains defective |
| HMA remote-client package filtering | PARTIAL | Process-local proxies in `PackageManagerHook.java` do not establish service-side Binder interception |
| HMA framework method backend / Settings hooks | MISSING | Ownership declarations are not installed hooks |
| PIF JNI Build assignments and native property hooks | IMPLEMENTED | `engines/pif/pif_engine.cpp`; actual callbacks and resulting values DEVICE_UNVERIFIED |
| PIF custom provider / effective LKG recovery | STUB | Provider flag has no provider implementation; recovery state population remains incomplete |
| TEE structural keybox inspection | PARTIAL | `engines/tee/tee_engine.cpp`; not cryptographic key/certificate validation |
| TEE attestation runtime | BLOCKED | Runtime activity remains false; supervisor/interceptor/certgen dependency chain absent |
| COPG profile selection, JNI and mapped-library PLT paths | PARTIAL | `engines/copg/copg_engine.cpp`; late-load coverage and measured application remain unverified |
| COPG namespace CPU mount path | PARTIAL | Companion mutation path exists; lifecycle, confinement and rollback gaps remain |
| Native config set/get/rollback/validate/status | IMPLEMENTED / DEVICE_UNVERIFIED | Native ingress, validated LKG and per-engine request generations; raw get and text mutation errors remain |
| Kaorios JSON storage | IMPLEMENTED | Allowlisted scalar Global mapping under `settingsGlobal`; not a Kaorios ROM runtime backend |
| Settings.Global config synchronization | IMPLEMENTED SUBSET / DEVICE_UNVERIFIED | On-demand fixed-argv reads and source-derived adapters; unsupported HMA/COPG semantics fail before publication; no write-back/watcher |
| Recoverable native transaction and rollback | IMPLEMENTED | Descriptor-relative redo journal and cooperating reader lock; arbitrary-reader atomic multi-file snapshots remain unsupported |
| Legacy migration / complete upstream schema migration | PARTIAL | Explicit exact-byte legacy migration/native metadata and COPG canonical copy implemented; not a general schema-version migration engine |
| Runtime generation/snapshot linkage | IMPLEMENTED / DEVICE_UNVERIFIED | HMA/PIF/COPG bind ACK v2 to actual consumer snapshot, request, boot and live process; Android transport/lifecycle and full runtime remain PARTIAL |
| Python and shell runtime parity | PARTIAL | Python status/runtime delegate to native; Action/boot scripts use native commands; legacy Python config persistence remains a separate domain |
| Global boot mutations | IMPLEMENTED | Removal in `ca95c37` follows requirement 8; no compatibility exception permits global property/settings rewrites |
| Android dual-ABI build/package definitions | IMPLEMENTED | Scripts and CI define ARM64/ARM32, API 31+ and DEX; artifact and loader success require separate evidence |
| Upstream identity automation | PARTIAL | Git objects verified; source integration, transitive dependency/license closure remain separate |
| Device acceptance | DEVICE_UNVERIFIED | No hardware results for Magisk, KernelSU or APatch |

At `bef592c`, local discovery passed 135 Python tests with one artifact-dependent skip; native build and CTest 1/1 passed. CI `43583721973` passed host and Android jobs, with all 135 artifact-integration tests running in the Android job. Only diagnostic artifact `14109666874` was uploaded; dependency/license acceptance and every hardware gate remain open. Counts are run-specific, not coverage percentages. See `DEVICE_TEST_RESULTS.md`.
