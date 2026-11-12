# Test Results and Evidence Boundary

## Checkpoint — 2026-11-12

| Evidence | Result | Qualification |
|---|---|---|
| Earlier host baseline | 36 native cases; 43 Python tests; one Python skip | Baseline counts, not measured coverage |
| Coordinating implementation run | 37 native cases; 59 Python tests; one Python skip | Reported verified by coordinating session; run-specific working-tree result, not independently rerun by this documentation pass |
| Commit reference | `ca95c37` | Callback containment, boot mutation removal and release hardening; later working-tree edits are separate |
| CI run `43219029337`, `ca95c37` | SUCCESS verified by coordinating session | Android ZIP artifact `13945066444`; not device acceptance |
| Final CI `43219803240`, `3045411` | SUCCESS verified by coordinating session | Host and Android jobs passed; no hardware acceptance implied |
| Android physical hardware | DEVICE_UNVERIFIED | No physical-device run or result recorded |

Historical 27-native/29-Python transcript excerpts are superseded as current counts. No all-tests transcript is invented here. Exact invocation, revision and output should accompany subsequent results; additional tests may change these numbers.

## What host evidence establishes

Host tests exercise dispatcher/configuration/tooling paths with mocked Zygisk and no-op JNI. Some baseline cases construct local expected states rather than call production failure paths. Packaging tests may skip real artifacts when absent. Passing these tests does not establish Android reflection, SELinux access, actual hook effects, Binder-client coverage, namespace confinement or Keystore non-interference.

The source audit reports in `/tmp/kaorios-audit-reports/0.txt` through `4.txt` were read-only inspections and did not execute tests. Git-object identity verification is source provenance evidence, not device execution or complete license/dependency closure.

## Required hardware results — all pending

| Gate | Required observation | Current result |
|---|---|---|
| Root/loader matrix | Magisk, KernelSU, APatch with exact Zygisk provider/version | DEVICE_UNVERIFIED |
| Platform/ABI | API 31+; ARM64 and ARM32 app/module paths | DEVICE_UNVERIFIED |
| Install/boot | Real library and companion load; stable system_server startup | DEVICE_UNVERIFIED |
| HMA | Independent client package queries and unchanged exempt clients | DEVICE_UNVERIFIED; service-boundary implementation incomplete |
| PIF | Actual configured JNI/native values and callback observations | DEVICE_UNVERIFIED; external verdicts are separate evidence |
| COPG | Target effects, late-library behavior, exact namespace/content and unchanged unrelated apps | DEVICE_UNVERIFIED |
| Runtime status | Current generation/boot/process identity after reload, failure, restart and reboot | BLOCKED generation/snapshot linkage: PIF/COPG emit 0, HMA emits 1; truthful CLI cannot converge to subsequent stored generations; hardware DEVICE_UNVERIFIED |
| Global mutation absence | Unrelated global properties/settings unchanged, including compatibility paths | DEVICE_UNVERIFIED; removal implemented under requirement 8 |
| Upgrade | Persistent configs preserved, stale runtime evidence excluded | DEVICE_UNVERIFIED |
| Uninstall/restore | Cleanup and documented backup recovery work | DEVICE_UNVERIFIED |
| Keystore | Ordinary key generation/sign/use unaffected before/after activation, upgrade and uninstall | DEVICE_UNVERIFIED |
| TEE runtime | Active backend and cryptographic acceptance | BLOCKED; structural keybox inspection only |
| Settings.Global synchronization | Defined mapping and measured application | BLOCKED; explicit unsupported, no settings changed |

## Recording the next result

Record commit and dirty-tree state, device/build/API, ABI, root-loader/Zygisk versions, artifact checksum, baseline, exact commands, target and unrelated-client observations, exit status and redacted logs. A diagnostic fixture running as a separate root process is not evidence that target-app hooks executed. `tools/test_fixture/fixture_main.cpp` is a diagnostic aid, not a complete acceptance harness.

No fabricated PASS row closes a pending gate. Configuration-file presence, DEX presence, an ACK counter or local proxy initialization are not end-to-end hardware proof.
