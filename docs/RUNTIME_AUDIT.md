# Runtime Audit

## Evidence checkpoint — 2026-11-14

The synchronization/persistence/ACK-v2 implementation starts at `bef592c`; the generation/COPG follow-up is based on `f4d1143`. The original five read-only reports for baseline `05a786f` are historical findings, not current execution results. The native follow-up replaces the earlier unsupported-sync and constant-generation paths. Remaining gaps below prevent an end-to-end completion claim. See [APP_MODULE_CONTRACT.md](APP_MODULE_CONTRACT.md) for exact implemented semantics.

## Implemented changes and limits

- `zygisk/src/dispatcher.cpp` now isolates engine callbacks with exception boundaries and records failures (`ca95c37`). This contains C++ exceptions, not arbitrary native faults, JNI errors or all system-server failures.
- `module/post-fs-data.sh` and `module/service.sh` remove global property/settings mutation paths under requirement 8. Compatibility is not an exception allowing global mutations back into boot scripts.
- Packaging/verification hardening in `ca95c37` improves required-payload and failure handling. A pipeline definition or host pass is not Android loader proof.
- NativeStore uses pinned-directory/no-follow operations, mandatory locking and a durable redo journal to recover grouped config/LKG/generation/request writes. Cooperating readers recover before reading; unlocked readers do not get multi-file atomicity.
- `config sync` reads allowlisted Global preferences through bounded direct-argv subprocesses and adapts a supported subset. It prevalidates a batch, merges supplied fields and performs no write-back. Two equal captures are not a provider transaction. Explicit legacy migration and validated rollback are implemented, not complete upstream schema migrations.
- `kaorios.json` stores mapped scalar Global preferences, not an implemented Kaorios runtime backend.
- ACK v2 binds exact consumer snapshot bytes and native generation to a fresh request, boot and live process identity. Strict parsing and monotonic publication reject stale/duplicate evidence. Later disk bytes never relabel an older producer snapshot. Python status/runtime compatibility delegates to native control rather than interpreting weaker ACKs independently.

## Generation/COPG audit follow-up

The following regressions were reproduced against the pre-fix implementation and covered through the actual compiled CLI/adapters:

| Finding | Follow-up behavior | Verification |
|---|---|---|
| D1: corrupt requested generation aborts boot preparation | Treat malformed requested metadata as stale; renew with the valid stored generation and fresh token. Invalid managed engines are reported without blocking valid-engine renewals; overall exit remains nonzero for those failures | Malformed/signed/overflow/trailing-junk counters, valid unrelated engine, invalid stored counter/payload |
| D2: corrupt stored generation aborts the complete status document | Engine-local `INVALID_CONFIG`, zero applied evidence, no automatic repair; ordinary mutations remain strict. Explicit migration validates exact payload bytes and starts a fresh generation-1 request identity | Stored/requested corruption, complete report, no metadata writes, strict mutation failures, explicit repair, stale ACK rejection |
| D3: sync shadows legacy-only COPG data | Require explicit migration before canonical publication, preserve legacy bytes/manual data, reject `sync all` before publishing other staged changes | Missing Settings no-op, rejected single/batch sync, migration then merge |
| D4: games-only COPG JSON overwrites stored enablement | Preserve existing `enabled` unless JSON enablement or scalar toggle is supplied; infer a default only if stored enablement is absent | Stored true/false, empty/nonempty games, explicit JSON/scalar combinations |
| D4b: semantic no-op wording exceeds implementation | Document equality of adapted JSON, not arbitrary runtime equivalence; existing HMA/PIF normalization may advance generation once | Scope sorting/deduplication and PIF flag conversion; repeated normalized sync leaves generation/request unchanged |
| F1/R2/R3: migration reports success without a payload; recovery guidance selects ineffective commands | Fail migration on corrupt stored metadata without usable payload; direct recovery through valid backup restoration, explicit unmanaged migration, supported runtime renewal or Kaorios config republication as appropriate | Missing/empty payload, single/all migration, no publication, storage-only requested corruption and generation-zero requested corruption |
| N1: old high-generation ACK blocks publication after counter repair to 1 | Companion and direct fallbacks hold config-to-runtime locks, validate current request/payload and supersede only different-token prior evidence. Current-request monotonic/failure-terminal checks and raw writer ordering remain | Real CLI migration plus persistent native ACK producer; canonical/compat-only prior ACKs, journal interruption/recovery, stale producer rejection, duplicate/terminal failure, unrelated PIF preservation |

The first local follow-up checkpoint passed 147 Python tests; independent diff review then found F1/R2/R3/N1 and regression tests reproduced them. After their fixes, full discovery passed 153 tests in 72.160s with one Android-artifact skip, native build, CTest 1/1 and 38 native cases. Exact-revision CI and Android transport/device acceptance are separate evidence. This fixes control/preservation/publication regressions, not the runtime gaps below.

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
| ACK transport | PARTIAL | Strict identity, peer checks and serialized publication exist; post-callback companion lifecycle and bounded framing still need Android qualification |
| Runtime generation/snapshot linkage | IMPLEMENTED / DEVICE_UNVERIFIED | Actual consumer snapshot hash/generation/request metadata replaces constant generation; unbound legacy snapshots produce no current proof |
| Observation/status | PARTIAL | Native status rejects stale boot/request/process evidence and requires successful initialization; one-shot reports still do not measure every hook effect |
| Persistence | IMPLEMENTED recoverability | Durable native journal plus reader/writer lock; not atomic publication for arbitrary raw readers |
| Migration/snapshot | PARTIAL | Explicit legacy metadata migration and native LKG rollback exist; complete upstream schema/version migration remains absent |
| Settings.Global sync | IMPLEMENTED SUBSET / DEVICE_UNVERIFIED | Supported adapters only; rich/default HMA policy can fail the whole batch before publication; permissions/SELinux untested |

Zygisk entry/registration must be evaluated against the actual macros and `internal::api_table` / `module_abi` definitions in `third_party/zygisk/zygisk.hpp`, not the old pseudocode signature. Host ABI self-consistency does not establish independent loader compatibility.

## Evidence boundary

At `bef592c`, local discovery ran 135 Python tests, passed with one artifact-dependent skip; native build and CTest 1/1 passed. Tests execute the real CLI, recovered snapshots, Settings subprocesses and live-process ACK fixtures, including producer metadata capture. Android hook/JNI behavior is still not exercised by host stubs. No measured line/branch coverage or hardware behavior is asserted.

CI `43583721973` passed host and Android jobs at that exact commit; Android artifact integration ran 135 tests without skips. Diagnostic artifact `14109666874` was uploaded, while release acceptance remained nonzero and release-accepted upload was skipped. Source provenance, dependency/license closure, Android build evidence and device acceptance are distinct.

Release gates remain Magisk / KernelSU / APatch, API 31+, ARM64 / ARM32, actual target-process effects, unchanged unrelated processes, reboot/reload identity, upgrade/uninstall preservation and unaffected Keystore key generation. All hardware gates are DEVICE_UNVERIFIED.
