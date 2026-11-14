# Source Audit 2

Current checkpoint: 2026-11-14, implementation `bef592c`. The 2026-11-12 audit below is retained as a historical record; its unsupported-sync, per-file-only persistence and constant-generation findings are superseded by the following implementation checkpoint.

## Current synchronization and evidence checkpoint

- **Settings.Global — implemented subset:** source-derived allowlists, bounded fixed-argv two-pass reads, explicit adapters/merge and batch prevalidation. No watcher or Settings write-back. HMA rich/default policy, Secure-only and alternate keybox sources are not silently converted; unsupported members fail before publication.
- **Persistence — implemented recoverability:** one locked, descriptor-pinned native store publishes a redo journal for config, validated LKG, generations and request identity. Cooperating readers recover first. Arbitrary unlocked readers still lack multi-file atomicity; trusted ancestors and complete semantic validation remain separate limitations.
- **Migration/rollback — partial overall:** explicit exact-byte legacy migration initializes native metadata, including optional COPG canonical copy; validated LKG rollback advances native generation. This is not a complete upstream schema-version migration framework. Legacy Python persistence has its own non-interchangeable counter/journal.
- **ACK v2 — implemented identity linkage:** HMA/PIF/COPG retain actual consumer bytes and bind their hash/generation to a fresh request, boot and live PID/EUID/start/argv0 identity. Strict parsing and serialized monotonic publication reject stale or regressive evidence. Same-generation reload renews request identity. Successful initialization is required before applied state; physical verification stays false.
- **Compatibility — native authority:** Python runtime/status invoke native control with fixed arguments/capability checks; absent control yields unavailable/zero evidence instead of raw ACK or marker fallback. App caller integration and Android permissions/lifecycle remain unverified.

At `bef592c`, local discovery passed 135 Python tests with one artifact-dependent skip, plus native build/CTest 1/1. CI `43583721973` passed host and Android jobs; Android artifact integration ran 135 tests without skips. Diagnostic artifact `14109666874` was uploaded; dependency/license acceptance remained nonzero and release-accepted upload was skipped. These are implementation/host/build results, not independent Android hook measurements or release/device acceptance.

Kaorios/TEE runtime backends, complete HMA service interception, COPG lifecycle/confinement/late-load coverage, ACK transport lifecycle and ScreenCapture remain incomplete or unsupported. See [APP_MODULE_CONTRACT.md](APP_MODULE_CONTRACT.md), [RUNTIME_AUDIT.md](RUNTIME_AUDIT.md) and [RELEASE_READINESS.md](RELEASE_READINESS.md). No physical hardware result has been recorded.

## Historical audit — 2026-11-12 (`ca95c37`)

The remaining sections describe that earlier checkpoint, not the current implementation.

## Method and coverage

Consolidated five source-only reports from `/tmp/kaorios-audit-reports/0.txt` through `4.txt`: control/configuration; native/Java runtime; build/package/vendor; upstream identities; cross-document consistency. Those reports inspected baseline `05a786f` and did not run tests or hardware. Their file/line references describe that baseline and may move after implementation.

This follow-up read all six updated architecture/parity/runtime/ownership/readiness/results documents, inspected the `ca95c37` change inventory, native configuration storage implementation and targeted current CLI/dispatcher/lifecycle code. It is not an independent line-by-line review of every source/vendor/transitive dependency. No code, README or decision log was changed by this documentation pass.

## Changes established

| Area | Status | Evidence and limit |
|---|---|---|
| Engine exception containment | IMPLEMENTED | Dispatcher callback boundaries in `ca95c37`; not universal native/Java crash prevention |
| Global boot mutation removal | IMPLEMENTED | Lifecycle script reduction in `ca95c37`; requirement 8 disallows a compatibility exception |
| Native config safety | PARTIAL | Working-tree NativeStore uses descriptor-relative no-follow/exclusive staging, mandatory lock and checked per-file fsync/rename; not a recoverable multi-file commit |
| Native JSON validation | IMPLEMENTED | Real parser/object requirement and selected type checks; full semantic schemas remain PARTIAL |
| Native config commands | PARTIAL | set/get/rollback/validate/status paths exist; runtime application is separate |
| Kaorios config persistence | IMPLEMENTED | Storage only; no Kaorios runtime backend established |
| Settings.Global synchronization | BLOCKED | Missing mapping; native command explicitly unsupported and mutates no settings |
| Complete schema migrations / snapshots | MISSING | No complete versioned migration or crash-atomic snapshot/restore contract |
| Release artifact guards | PARTIAL | `ca95c37` CI succeeded with ZIP artifact `13945066444` per coordinating verification; final CI `43219803240` at `3045411` also passed host/Android per coordinating verification |
| Upstream object provenance | IMPLEMENTED | Audit report 3 verified Git commit objects/tag resolutions; no implication of dependency/license closure |

## Open source findings

1. **HMA — PARTIAL:** DEX loader exists, contrary to stale log-only claims. Preferred Java entrypoint lacks native init/state methods, and process-local proxy replacement does not prove remote Binder filtering. SettingsProvider implementation remains MISSING.
2. **PIF — IMPLEMENTED / DEVICE_UNVERIFIED:** real property callbacks and JNI assignments exist. Provider behavior and effective recovery remain STUB. No integrity verdict is inferred.
3. **TEE — BLOCKED:** keybox shape parsing is not cryptographic key/certificate validation or an active attestation backend. Required supervisor/interceptor/certgen stack is absent.
4. **COPG — PARTIAL / DEVICE_UNVERIFIED:** JNI/PLT/mount paths exist, contrary to stale zero-implementation claims. Post-specialization companion use, namespace prerequisite handling, replacement recovery and late-load coverage remain unresolved.
5. **Runtime generation/snapshot linkage — BLOCKED:** PIF/COPG producers emit generation 0 and HMA emits 1; the truthful CLI cannot converge to subsequent stored generations. No complete unified runtime is established. Framing/concurrency, process/boot identity and one-shot observations also need repair. A valid config or matching file is not external device verification.
6. **Other control planes — PARTIAL:** Python host models and shell fallback semantics are separate from the deployed native contract. Existing baseline tests sometimes simulate locally constructed outcomes.
7. **Upstreams — PARTIAL:** KOW identity is distinct from PlayIntegrityFork; `pif.prop` is not local JSON compatibility. HMA/COPG schema adaptations are not drop-in parity. Exact grants, nested dependencies, release notices and source-to-artifact correspondence require closure.

## Test and release evidence

Coordinating run reported on 2026-11-12: **37 native cases; 63 Python tests; one Python skip**. Earlier baseline was **36 / 43 / one skip**. This documentation pass did not execute tests. Host mocks and no-op JNI limit those results; no line/branch coverage percentage is claimed.

Coordinating session subsequently verified CI run **`43219029337` for `ca95c37` succeeded**, producing Android ZIP artifact **`13945066444`**. Coordinating session then verified final CI **`43219803240` at `3045411` succeeded for host and Android jobs**. CI success is not device behavior proof.

No physical hardware tests were performed. Required acceptance remains **DEVICE_UNVERIFIED** for Magisk / KernelSU / APatch, API 31+, ARM64 / ARM32, install/boot/reload, target and unrelated-client effects, upgrade/uninstall preservation and unaffected Keystore key generation. Existing hooks are not end-to-end verified. This follow-up adds no bypass functionality and does not mark the mission complete.

## Documentation disposition

`FEATURE_PARITY.md` now separates source implementation from runtime proof. `RUNTIME_AUDIT.md` retains open findings without stale zero-code claims. `HOOK_OWNERSHIP.md` separates intended ownership from actual enforcement. `ARCHITECTURE.md` removes nonexistent socket/ROM bridge guarantees. `RELEASE_READINESS.md` replaces unsupported percentages with gates. `DEVICE_TEST_RESULTS.md` replaces stale fabricated-current transcripts with dated, attributed host counts and pending hardware gates.
