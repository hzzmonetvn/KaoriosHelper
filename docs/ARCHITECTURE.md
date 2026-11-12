# Architecture

Evidence checkpoint: 2026-11-12; `ca95c37` plus separately identified working-tree configuration/upstream changes. This describes implemented boundaries and open contracts, not a completion specification.

## Runtime and control boundaries

KaoriosHelper has a native Zygisk dispatcher, four engine adapters, a native `helperctl`, shell lifecycle scripts and host-side Python tooling. It is intended to support a root-mode frontend, but a complete Toolbox/ROM runtime bridge is not established.

| Source | Responsibility | Status / boundary |
|---|---|---|
| `zygisk/src/entry.cpp` | Module and privileged companion entrypoints | IMPLEMENTED; IPC/lifecycle and namespace checks remain PARTIAL |
| `zygisk/src/dispatcher.cpp` | Route engines and contain callback exceptions | IMPLEMENTED; exceptions isolated in `ca95c37`, not all crash classes |
| `engines/hma/`, `dex/src/` | Native DEX loading, Java policy/proxy scaffolding | PARTIAL; native/Java interface and service-boundary coverage unresolved |
| `engines/pif/` | Targeted JNI Build and native property paths | IMPLEMENTED / DEVICE_UNVERIFIED; provider/recovery incomplete |
| `engines/tee/` | Enablement and structural keybox inspection | PARTIAL; attestation runtime BLOCKED |
| `engines/copg/` | Per-app profile/JNI/PLT and companion mount paths | PARTIAL / DEVICE_UNVERIFIED |
| `cli/helperctl/helperctl.cpp` | Deployed native configuration/status commands | PARTIAL; status/ACK integration is not fully verified |
| `core/config/native_store.hpp` | Working-tree native persistence primitive | IMPLEMENTED single-file publication; not a multi-file transaction |
| `core/**/*.py` | Host configuration/runtime/status/registry/logging models | Separate contracts; not an Android Python runtime dependency |
| `module/` | Installation, boot, service, uninstall and CLI delegation | Hardware lifecycle qualification pending |
| `scripts/`, `.github/workflows/` | Build, packaging, artifact and upstream automation | Definitions are not successful-run evidence |

## Configuration contract

Default data directory: `/data/adb/modules/kaorioshelper/data/`.

| File | Purpose | Boundary |
|---|---|---|
| `pif.json` | Local PIF JSON adaptation | Not native KOW `pif.prop` compatibility |
| `hma.json` | Local HMA policy subset | Not full upstream schema parity |
| `keybox.xml` | TEE keybox input | Structure acceptance is not cryptographic validation or active attestation |
| `spoof.json` | Local COPG profile adaptation | Not demonstrated drop-in upstream schema compatibility |
| `kaorios.json` | Native Kaorios configuration storage | No runtime consumer / Settings.Global mapping established |
| Per-engine generation/LKG/pending files | Configuration revision and reload metadata | Not atomically committed as a group |
| Runtime ACKs / `status.json` | Reported runtime state | Needs current boot/process/config identity and actual observations |

The working-tree NativeStore pins a private directory descriptor, validates ownership/modes and file types, takes a mandatory lock, uses relative no-follow operations and exclusive staging, then fsync/rename/directory fsync for each publication. Rollback shares the lock. This narrows write hazards but does **not** establish crash atomicity across config, generation, backup and pending metadata. Complete versioned snapshots and schema migrations remain MISSING. JSON parsing/type checks are a subset of complete semantic validation; native XML checking is not cryptographic validation.

`helperctl config sync` explicitly reports Settings.Global synchronization unsupported because no native-config-to-ROM mapping exists. It changes no settings. Persisting `kaorios.json` must not be presented as a Kaorios runtime backend. A control socket or patched-ROM detection backend is not established by the audited sources.

## Execution and evidence flow

1. Zygisk dispatch selects engines using actual native process predicates, not the simplified policy model in Python.
2. Engines load configuration before privilege drop where implemented; HMA also stages DEX/configuration for Java startup.
3. Android-only hook/JNI paths attempt application. HMA proxy installation alone does not establish remote-client filtering; COPG's mapped-library scan does not establish late-load coverage.
4. Engine ACKs are intended to inform native status. Runtime generation/snapshot linkage is BLOCKED: PIF/COPG emit generation 0 and HMA emits 1, so the truthful CLI cannot converge to subsequent stored generations. Companion lifecycle, observation timing and identity validation also remain incomplete; this is not a complete unified runtime.
5. Configuration readiness, process loaded, hook registered, initialized, observed and externally verified are distinct evidence levels. No file-presence test should be treated as hardware verification.

Requirement 8 removes global boot property/settings mutations. Compatibility is not an exception to this boundary. Existing scoped hook paths are documented, not expanded; absent/forbidden functionality remains absent.

## Build and acceptance boundary

Android scripts/CI target API 31+, ARM64 and ARM32 with a DEX build path. Host JNI is a stub; host dispatcher tests use mocks. Historical toolchain blockers, host ELF labels and fabricated maturity percentages are not current proof. Verified upstream Git identities are distinct from complete source integration, transitive pinning and license-notice closure.

The coordinating 2026-11-12 run reported 37 native cases and 59 Python tests with one skip; the coordinating session verified CI `43219029337` for `ca95c37` succeeded with Android ZIP artifact `13945066444`; final CI `43219803240` at `3045411` subsequently passed host and Android jobs per coordinating verification. No device acceptance is recorded. Magisk / KernelSU / APatch qualification, upgrade/uninstall preservation and unaffected Keystore key generation remain DEVICE_UNVERIFIED. See `FEATURE_PARITY.md`, `SOURCE_AUDIT_2.md` and `DEVICE_TEST_RESULTS.md`.
