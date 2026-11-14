# App–module control contract

Updated 2026-11-14 for the Settings synchronization and ACK-v2 implementation on `refactor/kaorioshelper-2`. Control/status protocol remains version 1; ACK v2 is a separately negotiated capability. The Toolbox app caller and Android device integration are not yet verified.

## Ownership and transport

Native `helperctl` is the production ingress for module configuration and runtime requests. Invoke `/data/adb/modules/kaorioshelper/helperctl.sh` through the app's root execution facility with explicit argument boundaries. Do not use the obsolete `kaorios_helper` module id, a writable PATH lookup, direct data-file writes or direct generation/marker mutations.

Framework preferences and module runtime state remain separate domains. On-demand `config sync` reads an allowlisted Settings.Global subset and adapts it into module configuration; it does not write preferences back, infer Secure-only values or install a watcher. An app preference save alone is not module synchronization or application.

For `config set`/explicit-source validation, stage a regular file owned by the executing privileged UID with no group/world write permission. Native source reads reject final-component symlinks and are bounded to 16 MiB; ancestor directories must be trusted. Keep JSON/XML out of shell interpolation, logs, analytics and error reports. Remove app-created staging files after use.

## Commands and meanings

| Command | Response and meaning |
|---|---|
| `version --json` | Module id/version, control `protocolVersion: 1`, `statusSchemaVersion: 1`, boolean capabilities |
| `status --json` | `schemaVersion: 1`, `module`, `engines`; a recovered locked configuration view plus current runtime evidence |
| `config validate ENGINE [FILE]` | Exit 0/text on acceptance; without FILE validates stored canonical configuration; does not publish a new request |
| `config set ENGINE FILE` | Full validated replacement; preserve valid previous payload as LKG, advance stored/requested generation, create fresh request, pending runtime acknowledgement |
| `config rollback ENGINE` | Restore validated native LKG, advance generation and renew request; not runtime completion |
| `config sync ENGINE` / `config sync all` | Read selected Global keys, adapt/merge supplied fields, prevalidate every changed engine, then publish one recoverable transaction |
| `config migrate ENGINE` / `config migrate all` | Validate legacy payloads and preserve exact bytes while assigning missing native generation/request metadata; COPG may copy `COPG.json` into `spoof.json` |
| `config status ENGINE` | Configuration validation-only text, not synchronization/application state |
| `config get ENGINE` | Raw configuration payload; not a redacted metadata API |
| `enable ENGINE` / `disable ENGINE` | Record desired enablement and a fresh request in one transaction; restart may still be required |
| `reload ENGINE` / `reload all` | Fresh request token at the same stored generation; exit 0 means recorded, not applied |
| `prepare-boot` | Lifecycle entrypoint: renew stale boot requests for managed configurations; leave valid current-boot requests unchanged; do not migrate legacy payloads implicitly |

Configuration engines are `hma`, `pif`, `tee`, `copg`, `kaorios`. Runtime toggle/reload engines are `hma`, `pif`, `tee`, `copg`; `all` is supported for reload, sync and migration. HMA/PIF desired enablement defaults on unless disabled; TEE/COPG require opt-in. Kaorios configuration storage is not a functioning runtime backend. ScreenCapture is unsupported and not a registered operational engine.

Exit codes remain 0 for acceptance/success and 1 for errors/unsupported operations. Mutations return text, not structured JSON/error codes. Capture stdout, stderr and exit status separately. After an interrupted or failed publication, the outcome can be unconfirmed: a later cooperating read recovers a durable pending journal. Inspect status before an explicit retry; do not treat nonzero as proof that nothing changed.

Root-manager Action runs `config sync all` followed by status only if sync succeeds. `post-fs-data.sh` clears old runtime evidence and calls `prepare-boot` before Zygote consumption; `service.sh` only queries status once. There is no Settings polling loop.

## Capability negotiation

All capability values are boolean:

- True: `configSet`, `configValidate`, `configRollback`, `settingsGlobalSync`, `configMigration`, `recoverableConfigTransactions`, `runtimeAckV2`.
- False: `atomicMultiFileSnapshot`, `redactedConfigRead`, `structuredMutationErrors`, `deviceVerification`, `screenCapture`.

A true synchronization capability advertises the implemented adapter subset, not complete parity with every framework rule. Preserve unknown/false capabilities conservatively. ACK protocol version 2 does not change control/status schema version 1.

`config get` can expose private configuration. Do not use it for TEE/keybox display, logging or analytics. A redacted metadata API remains outstanding.

## Settings.Global adapters

The exact allowlists and conversions are in `core/config/settings_adapter.hpp`, derived from the current framework/app preference producers and consumers rather than invented ROM keys.

| Engine | Global keys and supported conversion |
|---|---|
| PIF | `kaorios_pif_json`: JSON object or supported property-text format; normalize recognized flag strings and merge supplied top-level fields |
| COPG | `kaorios_gameprops_json`, `kaorios_spoof_gameprops`: supported `games` map plus enabled/debug semantics; uppercase model/brand/manufacturer/device/product/fingerprint/release fields map to existing lowercase native profiles |
| HMA | `kaorios_hma_config`: scope arrays or representable simple blacklist AppRule subset with explicit unsupported-policy checks |
| TEE | `kaorios_keybox_xml`: active payload only; preserve exact bytes after structural validation |
| Kaorios | Fixed scalar allowlist under `settingsGlobal`, schema version 1; omitted manual fields remain untouched; storage only |

Kaorios scalar keys are limited to:

- Booleans: `kaorios_spoof_photos`, `kaorios_spoof_netflix`, `kaorios_spoof_disney_plus`, `kaorios_spoof_messenger_chathead`, `kaorios_spoof_pixel_apps`, `kaorios_spoof_meizu_music`, `kaorios_spoof_gms`, `kaorios_spoof_vending`, `kaorios_spoof_gameprops`, `kaorios_secure_flag`, `kaorios_hide_app_work`, `kaorios_hide_dev_work`, `kaorios_secure_flag_work`, `kaorios_keybox_enabled`, `kaorios_keybox_apply_all`, `kaorios_always_strong`, `kaorios_advanced_features`, `kaorios_hide_usb_charging`.
- Other scalars: `kaorios_keybox_active_profile` (`hw`/`rkp`), `kaorios_keybox_apply_all_mode` (`auto`/`leaf`/`gen`), `kaorios_security_patch`, `kaorios_manager_package`.

These stored names do not imply implemented native behavior for each preference. `kaorios_time` is a framework cache epoch, not a native generation, and is excluded. Secure-only and alternate keybox stores are excluded; selecting a scalar profile does not make TEE synchronization read a different payload source.

### Preservation and rejection rules

- `config set` replaces; `config sync` merges only fields supplied by its adapter. Unknown/manual existing fields and unrelated profiles remain. Touched HMA caller scope arrays are replaced by their converted list.
- Missing, whitespace, empty or literal `null` values mean no supplied preference, not deletion. JSON-semantic equality is a no-op; it does not renew a request or migrate generation-zero metadata. Use explicit migration/reload when needed.
- COPG retains manual CPU/PLT/Android-ID options and unrelated profiles. Unsupported incoming fields or `debug: true` fail rather than being silently dropped.
- HMA is not complete framework policy parity. Whitelist/opposite/preset/settings/installer semantics and enabled WebView protection are not converted. Ordinary app defaults may therefore fail validation; `sync all` publishes none if any member is invalid. A caller can select a supported engine without claiming that all preferences synchronized.
- Synchronization does not automatically change module runtime enablement. Stored COPG `enabled` and desired engine enablement are separate.
- TEE checks structure only, not complete XML validity or cryptographic validity, and has no complete runtime backend.

The fixed `/system/bin/settings` subprocess uses direct argv, redacted stderr, bounded output and a shared 30-second two-pass capture deadline. Each value is limited to 16 MiB and aggregate first-pass values to 32 MiB. Two equal reads detect many concurrent changes; they are not a provider-level atomic multi-key snapshot and do not prevent every ABA change.

## Persistence, migration and request identity

The shared `.config.lock` serializes native mutations, management readers, snapshot readers and companion identity checks. NativeStore pins the final directory, checks internal leaf ownership/mode/type/link count and uses private no-follow descriptor-relative staging. Trusted ancestor paths remain required.

A native `.native-transaction.json` redo journal groups configuration, validated native `.lkg`, stored/requested generations, request metadata and relevant markers. Durable journal publication commits the recovery decision; cooperating readers replay under lock before reading. Recovery is idempotent, but arbitrary raw/unlocked readers can see intermediate multi-file publication. This is recoverable persistence, not an atomic multi-file snapshot for every reader. Native journal values are bounded to 16 MiB each, 32 MiB in aggregate, at most 64 targets, and a 128 MiB serialized journal.

`ENGINE.stored_gen` and `ENGINE.requested_gen` are native per-engine counters. Legacy Python `generation.txt` and its `.config.transaction` journal are separate and not interchangeable or jointly recovered. Production clients must use native control rather than mix those writers.

Configuration presence does not invent generation 1. Explicit migration validates and preserves the old payload/schema, initializes missing native metadata and leaves the original `COPG.json` intact after a canonical copy. Already-managed payloads do not advance generation merely from migration. Rollback revalidates native LKG and creates a new generation.

Each request has a random 32-character lowercase hexadecimal token, current boot UUID and positive `CLOCK_BOOTTIME` lower-bound timestamp. Reload/toggle renew this identity even at the same stored generation; stale ACKs remain pending. Boot preparation renews prior-boot identity without increasing stored generation and does not silently migrate unmanaged payloads.

## Runtime evidence and status schema 1

Each native engine reports camelCase fields:

`installed`, `enabled`, `configValid`, `runtimeAvailable`, `processLoaded`, `hookRegistered`, `hookInitialized`, `runtimeObserved`, `deviceVerified`, `storedGeneration`, `requestedGeneration`, `appliedGeneration`, `lastError`, `lastRuntimeEvent`, `lastRuntimeTimestamp`, `lastVerifiedTimestamp`, `pendingReload`, `status`, `details`.

Distinguish desired state, stored config, accepted request and runtime evidence. Never infer application from exit 0, file presence, DEX loading, a verification timestamp or a requested generation alone.

Current ACK v2 evidence requires:

1. Matching engine and positive stored/requested/ACK generation.
2. SHA-256 of the exact consumer snapshot bytes and the current request token.
3. Current boot UUID, live non-zombie PID, matching effective UID, process start ticks and actual process name, plus the existing engine ownership restrictions.
4. A bounded boot-time ACK timestamp no earlier than process start or request creation.
5. Successful initialization/observation: both `initResult == 0` and `errorCode == 0`, with `HOOK_INITIALIZED` or `HOOK_OBSERVED`.

Strict parsing rejects malformed/missing fields, duplicate keys, excessive nesting, invalid ranges/events and legacy v1 evidence as current application. Publication rejects duplicate/regressive stages; failure is terminal within one process/request/snapshot. A different current request/process/snapshot can begin again. The companion verifies peer credentials and locked current state without restamping old producer evidence. Local evidence is not authentication against root or a compromised engine.

HMA/PIF/COPG ACK metadata describes the snapshot supplied to initialization/profile selection, not whichever file happens to be latest when the ACK is sent. An unbound legacy snapshot emits no current config-bound evidence. HMA no-argument initialization is not bound proof. Physical-device behavior remains unverified.

| State | Meaning |
|---|---|
| `DISABLED` | Desired enablement off; an already-running process may still need restart |
| `NOT_CONFIGURED` | Canonical config missing/invalid; legacy payload may require migration |
| `INCOMPLETE_RUNTIME` | Configuration/storage may exist, but the required backend/payload is incomplete |
| `PENDING_RELOAD` | Awaiting current identity-bound successful initialization; existing process may require restart |
| `FAILED` | Current matching process reported failure; zero applied generation |
| `READY` | Current matching initialization; observation still pending |
| `DEVICE_UNVERIFIED` | Current matching runtime observation; no external physical-device verification |

Old event/time fields may remain diagnostic after identity invalidation; they do not keep process/hook/applied evidence current. `deviceVerified` stays false and `lastVerifiedTimestamp` stays 0. Preserve unknown states conservatively in the app. `runtimeAvailable` is backend availability, not proof of device operation.

## Python compatibility

`StatusManager` and `RuntimeManager` call fixed-argv native control, negotiate protocol/capabilities and preserve native reports. Missing/incompatible/failing control returns unavailable state with zero applied evidence. They neither interpret raw ACKs nor mutate request/marker files directly; a status cache is output only.

Compatibility `get_runtime_ack_status()` maps native `READY`/`DEVICE_UNVERIFIED` to runtime-only `APPLIED`, never device verification. `trigger_reload()` now means request accepted and pending, not that an old ACK completed a reload. Legacy caller-supplied module-version/root-mode values do not overwrite native identity.

## Verification and outstanding integration

Host tests execute the real C++ CLI, adapters, recovered native snapshots and live-process ACK fixtures. Native producer metadata tests capture actual PIF/COPG submissions. Settings transport tests cover bounds, deadlines and descendant cleanup. Python compatibility tests call the real native CLI rather than fabricating a second status evaluator. Boot/Action shell fixtures verify delegation and error propagation.

These tests do not establish Android Settings permissions, root-manager lifecycle ordering, SELinux/proc visibility, ART hook behavior, app caller compatibility or device verification. Required next work includes app path/caller/parser integration, bounded privileged execution, redacted metadata, structured mutation errors, remaining backends and physical-device tests. Diagnostic CI ZIPs and release acceptance remain separate; upstream/license gates are unchanged.
