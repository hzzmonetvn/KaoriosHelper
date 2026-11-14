# Architecture

Evidence checkpoint: 2026-11-14, implementation `bef592c`. This describes implemented boundaries and open contracts, not a completion specification. Command and wire details are maintained in [APP_MODULE_CONTRACT.md](APP_MODULE_CONTRACT.md).

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
| `cli/helperctl/helperctl.cpp` | Production configuration/request/status ingress | IMPLEMENTED control paths; Android/app integration DEVICE_UNVERIFIED |
| `core/config/native_store.hpp` | Locked, descriptor-pinned native persistence | IMPLEMENTED recoverable redo-journal transactions; not arbitrary-reader multi-file atomicity |
| `core/config/settings_{command,adapter}.hpp` | Bounded Global reads and source-derived mappings | IMPLEMENTED supported subset; no watcher or write-back |
| `core/config/runtime_snapshot.hpp`, `include/ack_protocol.hpp` | Exact-byte snapshot and live-process request evidence | IMPLEMENTED host-tested ACK v2; Android transport/lifecycle remains PARTIAL |
| `core/**/*.py` | Host compatibility/configuration/registry/logging tools | Status/runtime delegate to native; legacy config store remains separate; not an Android Python dependency |
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
| `kaorios.json` | Allowlisted scalar Global preferences under `settingsGlobal` | Storage only; no Kaorios runtime consumer |
| Per-engine generation/LKG/request/pending files | Stored revision, validated recovery payload and fresh reload identity | Grouped in a recoverable native transaction |
| Runtime ACKs / `status.json` | Identity-bound runtime evidence / output cache | Native status is authoritative; cache is not fallback proof |

NativeStore pins the final private directory descriptor, validates leaf ownership/modes/type/link count, takes `.config.lock`, uses no-follow exclusive staging and checked fsync/rename/directory fsync. A durable `.native-transaction.json` redo journal groups payload, LKG, generations and request metadata. Cooperating native readers recover under lock before reading. Unlocked readers can still see intermediate publication; `atomicMultiFileSnapshot` remains false. Trusted ancestors are required. JSON checks cover selected schemas; native XML checking is structural, not cryptographic.

`helperctl config sync ENGINE|all` reads allowlisted Global keys twice through bounded fixed-argv subprocesses, adapts/merges supplied fields, validates the batch and publishes changed engines together. It does not write Settings, read Secure-only preferences, poll or silently convert unsupported policy. HMA rich/default rules may be unrepresentable and fail `sync all` before publication. Explicit `config migrate` preserves legacy bytes and assigns missing native metadata; validated rollback creates a new generation. This is not complete upstream schema migration. The legacy Python generation/journal domain is separate.

Persisting `kaorios.json` is not a Kaorios runtime backend. A control socket or patched-ROM detection backend is not established by these changes.

## Execution and evidence flow

1. Zygisk dispatch selects engines using actual native process predicates, not the simplified policy model in Python.
2. Engines load configuration before privilege drop where implemented; HMA also stages DEX/configuration for Java startup.
3. Android-only hook/JNI paths attempt application. HMA proxy installation alone does not establish remote-client filtering; COPG's mapped-library scan does not establish late-load coverage.
4. HMA/PIF/COPG retain the snapshot actually supplied to initialization/selection. ACK v2 binds its exact-byte hash, positive generation, fresh request token, boot and live process identity; the companion checks peer credentials/current locked state without restamping evidence. Unbound legacy snapshots do not prove current application. Reload and boot renewal invalidate old requests without inflating stored generation. Transport framing, companion lifecycle and actual Android observations remain incomplete.
5. Configuration readiness, process loaded, hook registered, initialized, observed and externally verified are distinct evidence levels. No file-presence test should be treated as hardware verification.

Requirement 8 removes global boot property/settings mutations. Compatibility is not an exception to this boundary. Existing scoped hook paths are documented, not expanded; absent/forbidden functionality remains absent.

## Build and acceptance boundary

Android scripts/CI target API 31+, ARM64 and ARM32 with a DEX build path. Host JNI is a stub; host dispatcher tests use mocks. Historical toolchain blockers, host ELF labels and fabricated maturity percentages are not current proof. Verified upstream Git identities are distinct from complete source integration, transitive pinning and license-notice closure.

At `bef592c`, local discovery ran 135 Python tests with one artifact-dependent skip; native build and CTest 1/1 passed. CI `43583721973` passed host and Android jobs, including 135 Android artifact-integration tests without skips. Diagnostic artifact `14109666874` is not release acceptance: dependency/license acceptance remains nonzero and release upload was skipped. Magisk / KernelSU / APatch qualification, actual app control calls, upgrade/uninstall preservation and unaffected Keystore key generation remain DEVICE_UNVERIFIED. See `DEVICE_TEST_RESULTS.md` for revision-specific evidence.
