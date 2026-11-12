# Release Readiness

## Verdict — 2026-11-12

**Engineering work in progress; device qualification incomplete.** Neither production readiness nor full upstream parity is established. Maturity percentages are not used because no measured coverage or hardware acceptance supports them.

Reference implementation commit: `ca95c37`. Configuration/upstream changes inspected in the working tree are not implied to be part of that commit. Coordinating session verified CI run `43219029337` for `ca95c37` **succeeded**, producing Android ZIP artifact `13945066444`. Final CI run `43219803240` at `3045411` subsequently **succeeded** for host and Android jobs, verified by the coordinating session. Device gates remain open.

## Follow-up implementation checkpoint

Baseline `7ae69c9` passed CI `43221053299`. The follow-up adds native ACK transport filesystem hardening, truthful Python ACK-based state, additional schema checks, and a source-free ScreenCapture capability that is disabled and unsupported. ScreenCapture is not registered as an operational engine; see `SCREENCAPTURE_AUDIT.md`.

CI now separates diagnostic artifacts from release-accepted artifacts. `upstreams.py acceptance` still exits nonzero for pending license/dependency evidence, so the release artifact upload is gated off. A diagnostic ZIP is build evidence only. No physical device is connected or qualified.

## Capability gates

| Gate | Status | Evidence / remaining work |
|---|---|---|
| Native dispatch and callback exception containment | IMPLEMENTED | `zygisk/src/dispatcher.cpp`, `ca95c37`; loader/device behavior still unverified |
| Global boot mutation removal | IMPLEMENTED | Requirement 8; no property/settings compatibility exception |
| Native configuration hardening | PARTIAL | NativeStore and JSON validation in working tree; not complete schema or multi-file transaction guarantees |
| Kaorios configuration | PARTIAL | JSON storage implemented; runtime Settings.Global backend/mapping absent |
| Settings.Global sync | BLOCKED | Explicit unsupported result with no settings changed |
| Snapshot / schema migration | MISSING | Complete crash-recoverable snapshot/restore and version migration not implemented |
| HMA runtime | PARTIAL | DEX build path exists; entrypoint and real remote-client interception remain unresolved |
| PIF runtime | DEVICE_UNVERIFIED | Native hook/JNI paths exist; host mocks are not actual Android measurements |
| TEE runtime | BLOCKED | Keybox structure inspection only; backend dependency chain absent |
| COPG runtime | PARTIAL | Lifecycle, namespace confinement and late-library coverage gaps |
| Runtime generation/snapshot linkage | BLOCKED | PIF/COPG emit generation 0 and HMA emits 1; truthful CLI cannot converge to subsequent stored generations; no complete unified runtime |
| Upstream identity | IMPLEMENTED | Verified Git objects; not proof of vendored integration or release-asset equivalence |
| Dependency/license distribution closure | PARTIAL | Transitive pins, exact grants and packaged notices require separate closure |
| Android artifacts | PARTIAL | `ca95c37` CI succeeded with ZIP artifact `13945066444`; final CI `43219803240` at `3045411` passed host/Android; hardware unverified |
| Physical acceptance | DEVICE_UNVERIFIED | No hardware execution results |

## Test checkpoint

Coordinating host execution on 2026-11-12 reported **37 native cases, 63 Python tests, one Python skip**. Earlier baseline: 36 native, 43 Python, one skip. Counts can change with subsequent patches; preserve exact commands and logs with each run. This documentation pass did not execute the suites. Host JNI and Zygisk mocks limit what these results establish.

## Mandatory release evidence

1. Verify the exact release commit and successful CI run; inspect newly built ZIP, both ABI libraries/CLIs, DEX, permissions, exports, dependencies and applicable page alignment. Confirm no private configuration/key material is packaged.
2. Close upstream source/dependency/license inventories independently from Git-object identity verification.
3. Resolve HMA entrypoint/service boundary, runtime ACK transport/generation semantics and COPG confinement/lifecycle gaps. Keep absent runtime capabilities explicit rather than manufacturing verification markers.
4. Exercise **Magisk, KernelSU and APatch**, **API 31+**, **ARM64 and ARM32**, recording exact OS/root-loader versions and supported Zygisk provider. Framework names alone are not compatibility proof.
5. Verify actual independent-client effects, unchanged unrelated processes, reload/reboot/process-death status, and error containment.
6. Verify upgrade preservation, uninstall/restore behavior and **unaffected Keystore key generation** on hardware. Host non-interference tests do not close these gates.

No hardware test has passed by implication. Existing implementation remains separate from forbidden or absent functionality; this pass introduces no new bypass feature. See `DEVICE_TEST_RESULTS.md` and `SOURCE_AUDIT_2.md`.
