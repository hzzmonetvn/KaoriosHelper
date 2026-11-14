# Release Readiness

## Verdict — 2026-11-14

**Engineering work in progress; device qualification incomplete.** Neither production readiness nor full upstream parity is established. Maturity percentages are not used because no measured coverage or hardware acceptance supports them.

Reference implementation: `bef592c`. [CI 43583721973](https://github.com/hzzmonetvn/KaoriosHelper/actions/runs/43583721973) passed host and Android jobs at that commit. Diagnostic artifact `14109666874` was uploaded; release-accepted upload was skipped because dependency/license acceptance remains nonzero. Device gates remain open. A green diagnostic workflow is not release acceptance.

## Follow-up implementation checkpoint

`bef592c` adds source-derived on-demand Global adapters, recoverable native config/LKG/request transactions, explicit legacy migration, fresh same-generation reload requests and snapshot-bound live-process ACK v2. Python status/runtime compatibility now delegates to native control. Supported mapping and evidence semantics are in `APP_MODULE_CONTRACT.md`; full schema/runtime parity is not implied. ScreenCapture remains disabled/unsupported and is not registered as an operational engine; see `SCREENCAPTURE_AUDIT.md`.

CI now separates diagnostic artifacts from release-accepted artifacts. `upstreams.py acceptance` still exits nonzero for pending license/dependency evidence, so the release artifact upload is gated off. A diagnostic ZIP is build evidence only. No physical device is connected or qualified.

## Capability gates

| Gate | Status | Evidence / remaining work |
|---|---|---|
| Native dispatch and callback exception containment | IMPLEMENTED | `zygisk/src/dispatcher.cpp`, `ca95c37`; loader/device behavior still unverified |
| Global boot mutation removal | IMPLEMENTED | Requirement 8; no property/settings compatibility exception |
| Native configuration hardening | IMPLEMENTED / DEVICE_UNVERIFIED | Descriptor-pinned locked redo-journal recovery and validated LKG; not arbitrary-reader multi-file atomicity or complete semantic schemas |
| Kaorios configuration | PARTIAL | Scalar Global adapter/storage implemented; runtime backend absent |
| Settings.Global sync | IMPLEMENTED SUBSET / DEVICE_UNVERIFIED | On-demand allowlisted read/merge; no watcher/write-back; unrepresentable policy fails before batch publication |
| Snapshot / schema migration | PARTIAL | Exact-byte legacy migration, native metadata initialization and validated rollback implemented; not complete upstream schema-version migration |
| HMA runtime | PARTIAL | DEX build path exists; entrypoint and real remote-client interception remain unresolved |
| PIF runtime | DEVICE_UNVERIFIED | Native hook/JNI paths exist; host mocks are not actual Android measurements |
| TEE runtime | BLOCKED | Keybox structure inspection only; backend dependency chain absent |
| COPG runtime | PARTIAL | Lifecycle, namespace confinement and late-library coverage gaps |
| Runtime generation/snapshot linkage | IMPLEMENTED / DEVICE_UNVERIFIED | Actual snapshot hash/generation, request, boot and live-process ACK identity; transport/lifecycle and full runtime coverage remain PARTIAL |
| Upstream identity | IMPLEMENTED | Verified Git objects; not proof of vendored integration or release-asset equivalence |
| Dependency/license distribution closure | PARTIAL | Transitive pins, exact grants and packaged notices require separate closure |
| Android artifacts | BUILD VERIFIED / RELEASE BLOCKED | `bef592c` CI `43583721973` produced diagnostic artifact `14109666874`; release/license acceptance and hardware gates remain open |
| Physical acceptance | DEVICE_UNVERIFIED | No hardware execution results |

## Test checkpoint

The `bef592c` local run passed **135 Python tests with one artifact-dependent skip**, native build and CTest 1/1. Its Android CI artifact integration ran **135 tests without skips**. Host tests cover the real CLI, Settings subprocesses, journal recovery and live-process/snapshot ACK fixtures; JNI/Zygisk stubs are not device measurements. Preserve revision, commands and logs with every run; these counts are not coverage percentages.

## Mandatory release evidence

1. Verify the exact release commit and successful CI run; inspect newly built ZIP, both ABI libraries/CLIs, DEX, permissions, exports, dependencies and applicable page alignment. Confirm no private configuration/key material is packaged.
2. Close upstream source/dependency/license inventories independently from Git-object identity verification.
3. Resolve HMA entrypoint/service boundary, remaining ACK transport/lifecycle and COPG confinement/late-load gaps. Verify the implemented snapshot/request identity on Android and the Toolbox caller/parser against the deployed control contract. Keep absent runtime capabilities explicit rather than manufacturing verification markers.
4. Exercise **Magisk, KernelSU and APatch**, **API 31+**, **ARM64 and ARM32**, recording exact OS/root-loader versions and supported Zygisk provider. Framework names alone are not compatibility proof.
5. Verify actual independent-client effects, unchanged unrelated processes, reload/reboot/process-death status, and error containment.
6. Verify upgrade preservation, uninstall/restore behavior and **unaffected Keystore key generation** on hardware. Host non-interference tests do not close these gates.

No hardware test has passed by implication. Existing implementation remains separate from forbidden or absent functionality; this pass introduces no new bypass feature. See `DEVICE_TEST_RESULTS.md` and `SOURCE_AUDIT_2.md`.
