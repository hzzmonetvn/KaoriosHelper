# App–module control contract

Status: proposed integration agreement based on module commit `12ac8fd`, 2026-11-12. App peer: KaoriosToolbox-Frameowrk, branch `test/rust-app-nativepif-patcher`. App implementation and device integration are not yet verified.

## Ownership and transport

The native helperctl is the sole ingress for module configuration. Existing framework Settings.Global/Secure preferences remain a separate domain: no automatic mirroring, and no assumed Settings.Global synchronization. The app must not write module data files or generation markers directly.

Invoke the installed module's `helperctl.sh` through the app's root execution facility, using explicit argument boundaries. Resolve the installed module path, not a writable PATH entry. Do not interpolate JSON, arbitrary engine names or filenames into a shell command. Source files must be root-owned regular files with no group/world write permission; the native source reader rejects symlinks. Keep payloads out of logs and delete app-created staging files after use.

## Existing commands

| Command | Current response and meaning |
|---|---|
| `version --json` | JSON containing `moduleId`, `version`, `protocolVersion: 1`, `statusSchemaVersion: 1`, and boolean `capabilities` |
| `status --json` | JSON `schemaVersion: 1`, `module`, and `engines`; read-only observation |
| `config validate ENGINE FILE` | Exit 0 and text on validator acceptance; does not publish |
| `config set ENGINE FILE` | Full replacement, validated publication, LKG backup when valid; generation advances; does not establish application |
| `config rollback ENGINE` | Restores validated LKG and advances generation; pending runtime acknowledgement |
| `config status ENGINE` | Validation-only text result, not synchronization state |
| `enable ENGINE` / `disable ENGINE` | Desired-state marker change, reload pending; not observed runtime completion |
| `reload ENGINE` / `reload all` | Exit 0 means request recorded, not reload completed |
| `config sync ENGINE` | Unsupported, exit 1; no settings changed |

Configuration engines: `hma`, `pif`, `tee`, `copg`, `kaorios`. Runtime toggle/reload engines: `hma`, `pif`, `tee`, `copg`. Kaorios storage is not an implemented runtime backend. ScreenCapture is unsupported and not a registered operational engine.

`config get` currently prints raw configuration and can expose keybox secrets. The app must not use it for TEE/keybox display, logging or analytics. A redacted metadata API remains required; do not advertise a secret-safe read API that does not exist.

Current exit codes are 0 for command acceptance/success and 1 for errors/unsupported operations. Machine-readable per-error codes are not implemented. `--json` is implemented for status/version, not all mutations. App must capture stdout, stderr and exit code separately and handle malformed/unknown JSON conservatively.

## Status schema 1

Wire names are camelCase. Each existing engine reports `installed`, `enabled`, `configValid`, `runtimeAvailable`, `processLoaded`, `hookRegistered`, `hookInitialized`, `runtimeObserved`, `deviceVerified`, `storedGeneration`, `requestedGeneration`, `appliedGeneration`, `lastError`, `lastRuntimeEvent`, `lastRuntimeTimestamp`, `lastVerifiedTimestamp`, `pendingReload`, `status`, `details`.

App state must distinguish desired enablement, stored configuration, pending application and runtime evidence. Never infer ACTIVE/applied from exit 0, config presence, DEX loading, a timestamp marker or requested generation alone. Exact engine and positive stored/requested/ACK generation plus successful initialization/observation are required by current native status logic. `deviceVerified` remains false.

Do not map `READY` to externally verified. Preserve `DEVICE_UNVERIFIED`, `PENDING_RELOAD`, `FAILED`, `INCOMPLETE_RUNTIME`, disabled/unconfigured states and unknown states in the UI. Existing engine producer generations do not generally converge yet: PIF/COPG emit 0, HMA emits 1. ACK freshness lacks boot/process-start/configuration-hash binding.

## Acceptance fixtures and outstanding work

`tests/test_native_helperctl.py` compiles and exercises the real native CLI, including replacement/rollback, generation, validation, unsupported sync and stale/failed/mismatched ACK fixtures. `tests/test_runtime_manager.py` and `tests/test_status_manager.py` cover Python compatibility behavior but are not an Android transport implementation.

Required next integration work: app caller inventory, explicit capability/protocol negotiation, stable error codes, redacted configuration metadata, bounded root process execution, app parser tests against native output, generation-consumer linkage and actual device tests. Per-file publication is not a crash-atomic multi-file snapshot; app must not claim otherwise. Python ConfigManager's global generation.txt is not interchangeable with native per-engine generation metadata.
