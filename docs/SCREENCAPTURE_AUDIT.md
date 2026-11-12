# ScreenCapture upstream audit

Date: 2026-11-12. Audited LSPosed/DisableFlagSecure at immutable commit `7cf0f1fd826dad1e419cc02a2ef94c4eba591be1` using read-only GitHub source inspection. No device testing was performed.

## Dependencies and runtime contract

The upstream is a Java libxposed module, not a Zygisk native library. Its build declares `io.github.libxposed:api:102.0.0` and `androidx.annotation:annotation:1.10.0` as compile-only dependencies, Java 21, minimum Android API 31, target 37, compile SDK 37 minor 1 and build-tools 36.1.0. Module metadata declares minimum API 101, target 102, static scope and automatic hot reload.

Its GPL v3 license text needs distribution review before source reuse; this audit does not infer an only/or-later qualifier absent explicit source grants. No upstream implementation was copied into this increment.

## API and OEM differences

The source distinguishes Android S/T, U, V and Baklava minor 1. Capture APIs move between SurfaceControl, ScreenCapture and ScreenCaptureInternal; hidden method and field shapes differ. OEM branches cover Samsung OneUI, Xiaomi HyperOS/MIUI Screenshot, Oplus Screenshot/AppPlatform/ScreenRecorder and Flyme SystemUIEx. Static scope includes system and six associated packages. Upstream comments describing tested versions are not local device evidence.

## Portability blockers

The existing unified dispatcher exposes Zygisk specialization callbacks, not libxposed package/system-server-ready callbacks, Java method interception, ART deoptimization or hot-reload ownership. Loading DEX or bundling the libxposed API does not implement those capabilities. A direct upstream port is therefore BLOCKED, not operational.

## Supported integration boundary

ScreenCapture remains optional and disabled by default. Capability reporting must distinguish desired enablement from backend availability and must never generate a successful runtime ACK for class loading or process selection. Cooperative test applications may implement capture through their own ordinary platform-consent flow. No system-server/OEM secure-content interception or screenshot-observer suppression is introduced.

The capability contract is source-free and reports an unavailable backend until a real cooperative test-app handshake is implemented and verified. This is not a functioning DisableFlagSecure port or proof of screen-capture support.
