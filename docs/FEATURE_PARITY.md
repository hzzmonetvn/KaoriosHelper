# Feature Parity

Evidence checkpoint: 2026-11-12, committed baseline `ca95c37` plus in-progress native configuration/upstream changes. This is source capability classification, not an end-to-end or hardware acceptance report.

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
| Native config set/get/rollback/validate/status | PARTIAL | `helperctl.cpp`, `core/config/native_store.hpp`; descriptor-relative persistence and JSON parsing added in working tree |
| Kaorios JSON storage | IMPLEMENTED | `kaorios.json` persistence only; not a Kaorios ROM runtime backend |
| Settings.Global config synchronization | BLOCKED | Mapping absent; native sync explicitly reports unsupported and changes no settings |
| Single-file replacement and locked rollback | IMPLEMENTED | NativeStore uses no-follow/exclusive staging, fsync and rename; multi-file crash atomicity is not established |
| Versioned snapshots / complete schema migrations | MISSING | No recoverable multi-file manifest or complete migration pipeline |
| Runtime generation/snapshot linkage | BLOCKED | PIF/COPG producers emit generation 0 and HMA emits 1; truthful CLI cannot converge to subsequent stored generations; no complete unified runtime |
| Python and shell runtime parity | PARTIAL | Separate host models/fallback semantics; not equivalent to deployed native behavior |
| Global boot mutations | IMPLEMENTED | Removal in `ca95c37` follows requirement 8; no compatibility exception permits global property/settings rewrites |
| Android dual-ABI build/package definitions | IMPLEMENTED | Scripts and CI define ARM64/ARM32, API 31+ and DEX; artifact and loader success require separate evidence |
| Upstream identity automation | PARTIAL | Git objects verified; source integration, transitive dependency/license closure remain separate |
| Device acceptance | DEVICE_UNVERIFIED | No hardware results for Magisk, KernelSU or APatch |

Host checkpoint reported by the coordinating run on 2026-11-12: 37 native cases and 59 Python tests, one Python skip. Earlier baseline was 36 native / 43 Python / one skip. These counts are run-specific, not coverage percentages. Coordinating session subsequently verified CI run `43219029337` for `ca95c37` succeeded, with Android ZIP artifact `13945066444`. Coordinating session then verified final CI `43219803240` at `3045411` succeeded for host and Android; hardware acceptance remains unverified. See `SOURCE_AUDIT_2.md` and `DEVICE_TEST_RESULTS.md`.
