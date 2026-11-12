# Native configuration decisions

## Implemented choices

- Extend the existing native `HelperCtl` interface rather than introduce a daemon or device-side Python dependency.
- Parse JSON using the existing bundled parser, require objects, validate known engine field types, and preserve arbitrary unknown fields present in the supplied payload.
- Retain `config set` full-replacement semantics: omitted old fields are deliberately removed. Rollback restores the validated LKG payload rather than merging it.
- Support `kaorios.json`; an optional `schemaVersion` must be integer 1. PIF/COPG `enabled`, when present, must be boolean.
- Serialize native set/rollback/get with a checked exclusive lock and pin the final data directory with a descriptor. Require effective-UID ownership and no group/world write permissions. Production Android execution is expected to use root.
- Use descriptor-relative, no-follow internal file access, regular-file checks, exclusive staging creation, checked bounded writes, file fsync, rename and directory fsync. Set and explicit-source validation share a nonblocking/no-follow, bounded regular-file reader.
- Save an LKG only when the existing payload passes its engine validator; validate the LKG again before rollback.
- Invalidate legacy verification evidence when publishing configuration. Local configuration, payload artifacts and legacy timestamp markers never establish external device verification.
- Reload records the stored generation as requested without advancing configuration generation, polling or claiming application success.
- Only exact engine/stored/requested-generation ACK matches with explicit successful initialization/observation events establish applied state. Failure events do not imply initialized state. Use FAILED, READY and DEVICE_UNVERIFIED status labels while retaining the existing hookInitialized field.

## Blocked capabilities and explicit limitations

- **Multi-file snapshot transactions and crash recovery: BLOCKED / not implemented.** Individual file replacements are durable and atomic, but the LKG, verification invalidation, reload marker, requested generation, stored generation and payload are not one transaction. Metadata-first publication can leave advanced generations and pending state with the old payload after an interruption or I/O failure. No journal or automatic recovery is implemented.
- **Settings.Global synchronization: BLOCKED / unsupported.** Repository inspection found no native-configuration-to-ROM Settings.Global mapping. The explicit sync command returns an error and changes no settings. No ROM keys were invented. Preservation rules for a future adapter remain future work.
- **XML validation: PARTIAL.** The existing structural substring validator remains; this is not a complete XML parser or cryptographic/keybox validation.
- Parent path components of the configured data/source paths are not protected by component-by-component no-follow traversal. Those ancestors must be trusted. The final data directory and internal file leaves are checked.
- Status readers do not take the configuration lock and therefore do not obtain a coherent multi-file snapshot. Existing toggle/status-cache helpers retain their earlier direct-path implementation. `config status` reports configuration validation, not synchronization state.
- The existing ACK reader lacks boot identity, process-start identity and configuration-hash binding and follows ACK path symlinks. Exact generation matching alone does not prevent same-generation replay.
- Current PIF/COPG producers report generation 0 and HMA reports hardcoded generation 1, so general persisted snapshot-generation convergence is unsupported. Mismatches retain pending state and explicitly report `Runtime snapshot-generation acknowledgement unsupported by current engine` rather than claiming successful application.
- No external device-probe provider is implemented; deviceVerified remains false. No interception hooks, forged attestation, concealment or third-party check bypass functionality was expanded.

## Alternatives not chosen

- Recursive merge for `config set` was removed because it changes existing replacement semantics and prevents deliberate field removal.
- Relaxing ACK generation matching was not chosen because it would report stale or unrelated runtime state as applied.
- Reporting local readiness as device verification, inventing Settings.Global mappings, and claiming a multi-file transaction from individual renames were not chosen because the implementation does not establish those guarantees.

## Verification actually performed

- `python3 -I tests/test_native_helperctl.py`: 9 integration tests passed, compiling and executing the actual native CLI with `g++ -std=c++20 -Wall -Wextra`.
- Coverage includes malformed/type-invalid JSON, replacement semantics, unknown-field payload preservation, validated rollback, generations, concurrent writers, source/internal/directory symlinks, writable-directory rejection, FIFO validation, unknown engines, stale/failed/mismatched ACKs, truthful verification and unsupported synchronization.
- `git diff --check` passed before the final compiler-standard-only test update.
- No Android device execution, reboot persistence demonstration, injected I/O-failure testing or external device verification was performed by this implementation task.
