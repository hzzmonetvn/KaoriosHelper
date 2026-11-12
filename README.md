# KaoriosHelper

**Unified Android Root & Zygisk Framework**  
*Consolidating Package Hiding (HMA), Play Integrity (PIF), Hardware Attestation (TEE), and Game Profiling (COPG) under a single native Bionic runtime.*

[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL_3.0-blue.svg)](https://www.gnu.org/licenses/agpl-3.0.en.html)
[![NDK: r26b](https://img.shields.io/badge/NDK-r26b%20(API%2031)-green.svg)](https://developer.android.com/ndk)
[![ABI: arm64--v8a%20%7C%20armeabi--v7a](https://img.shields.io/badge/ABI-arm64--v8a%20%7C%20armeabi--v7a-orange.svg)]()
[![Status: ALPHA](https://img.shields.io/badge/Status-ALPHA%20(Dev%2FLab)-yellow.svg)]()
[![Device Status: DEVICE_NOT_TESTED](https://img.shields.io/badge/Hardware-DEVICE__NOT__TESTED-red.svg)]()

---

## 1. Overview & Architecture

KaoriosHelper is a modern, unified Zygisk module engineered to eliminate conflicts, hook collisions, and memory overhead caused by running multiple independent root and hooking modules concurrently.

```
                      ┌───────────────────────────────────────────────┐
                      │              Android Zygote Init               │
                      └──────────────────────┬────────────────────────┘
                                             │
                      ┌──────────────────────▼────────────────────────┐
                      │    KaoriosHelper Dispatcher (Zygisk ABI v4)   │
                      └───────┬──────────────┬──────────────┬─────────┘
                              │              │              │
      ┌───────────────────────┼──────────────┼──────────────┼────────────────────────┐
      │                       │              │              │                        │
┌─────▼───────────────┐ ┌─────▼────────┐ ┌───▼────────┐ ┌───▼────────────────┐ ┌─────▼────────────────┐
│     HMA Engine      │ │  PIF Engine  │ │ TEE Engine │ │   COPG Engine      │ │ Native Control Plane │
│  (system_server)    │ │ (GMS/libc)   │ │  (Keybox)  │ │ (Games/Virtual)    │ │     (helperctl)      │
├─────────────────────┤ ├──────────────┤ ├────────────┤ ├────────────────────┤ ├──────────────────────┤
│ • PMS filtering     │ │ • ShadowHook │ │ • Keybox   │ │ • /proc/cpuinfo    │ │ • Pure C++20 CLI     │
│ • SettingsProvider  │ │   inline hook│ │   validation │   setns mount (slave)│ • Atomic staging       │
│ • InMemoryDexLoader │ │ • Verified   │ │ • GMS target │ • ANDROID_ID cache   │ • flock(LOCK_EX)       │
│ • Direct ByteBuffer │ │   boot state │ │   confinement│   poisoning          │ • Multi-ABI (arm64/    │
│ • (ALPHA: Scaff.)   │ │ • (BETA)     │ │ • (ALPHA)  │ │ • (BETA)           │ │   armeabi-v7a)       │
└─────────────────────┘ └──────────────┘ └────────────┘ └────────────────────┘ └──────────────────────┘
```

---

## Current verification checkpoint — 2026-11-12

The 2.0 working branch is an incremental hardening release, not a completed architecture refactor. Current evidence and blockers are maintained in [SOURCE_AUDIT_2.md](docs/SOURCE_AUDIT_2.md), [FEATURE_PARITY.md](docs/FEATURE_PARITY.md), and [RELEASE_READINESS.md](docs/RELEASE_READINESS.md). Native configuration validation and persistence, failure containment, upstream update proposals, and release packaging have been improved. Settings.Global synchronization, complete snapshot migration, runtime generation linkage, and the Kaorios framework/Zygisk backend remain incomplete. No physical-device validation has been performed.

The older architectural descriptions below are historical context, not proof of current runtime functionality. Boot-wide property mutations have been removed; KOW Inject is pinned separately from PlayIntegrityFork and its `pif.prop` format is not implemented as a compatible adapter.

## 2. Subsystems & Maturity Matrix

Under strict engineering truthfulness protocols, KaoriosHelper is classified as **ALPHA (Developer / Lab Test Build)**. Production readiness cannot be claimed until physical device field testing is completed (`DEVICE_NOT_TESTED`) and mandatory runtime components are fully resolved.

| Subsystem | Upstream Ancestor | Maturity | Hook & Runtime Mechanism | Device Status |
|---|---|---|---|---|
| **Zygisk Core** | `topjohnwu/zygisk-module-sample` | **PARTIAL** | Official Zygisk ABI v4, `dlclose` unmanaged, `SO_PEERCRED` namespace isolation | `DEVICE_NOT_TESTED` |
| **Control Plane** | Native C++ Rewrite | **PARTIAL** | Pure C++20 CLI (`helperctl`), multi-ABI, atomic file staging, `flock(LOCK_EX)` | `DEVICE_NOT_TESTED` |
| **PIF Engine** | `osm0sis/PlayIntegrityFork` | **DEVICE_UNVERIFIED** | ShadowHook inline hook on Bionic libc `__system_property_read_callback` | `DEVICE_NOT_TESTED` |
| **COPG Engine** | `07c6482e` (COPG) | **PARTIAL** | Peer `setns` mount (`MS_SLAVE`), client-side `ANDROID_ID` cache poisoning | `DEVICE_NOT_TESTED` |
| **TEE Engine** | `TrickyStore` / `AlwaysStrong` | **BLOCKED** | Keybox XML parser active; Keystore2 ptrace daemon external; disabled by default | `DEVICE_NOT_TESTED` |
| **HMA Engine** | `frknkrc44/HMA-OSS` | **PARTIAL** | Native DEX loader and CI-packaged DEX; entrypoint/service boundary unresolved | `DEVICE_NOT_TESTED` |

---

## 3. Honest Runtime Blockers & Technical Analysis

### 3.1 HMA Engine: Runtime Integration Blockers
- **Build evidence**: CI builds and packages `classes.dex` using a JDK and Android SDK build-tools. Missing DEX is a packaging failure, not the current compilation blocker.
- **Runtime gaps**: The preferred Java entrypoint does not implement the native-expected initialization/state contract. Process-local proxies do not establish remote Binder behavior.
- **Qualification**: Host tests and a compiled DEX do not establish successful system_server initialization or boot stability.

### 3.2 TEE Engine: Keystore2 System Daemon Isolation
- **Validation limit**: Keybox inspection checks selected XML structure only; it does not establish cryptographic key/certificate validity or certificate-chain trust.
- **Runtime gap**: Keystore2 is outside the Zygote lifecycle. The required backend dependency chain is absent; XML configuration alone does not establish an active TEE runtime.
- **Safe Default**: TEE is **disabled by default** (`enabled: false`) to safeguard Android lockscreen PIN decryption, biometric enrollment, and hardware key generation. XML parsing is not claimed as genuine hardware attestation.

### 3.3 COPG Engine: Mount Propagation Hardening
- **Namespace Virtualization**: Uses `getsockopt(SOL_SOCKET, SO_PEERCRED)` to obtain the client PID and switches into the target mount namespace via `setns(open("/proc/<pid>/ns/mnt"), CLONE_NEWNS)`.
- **Open confinement finding**: Namespace prerequisites and propagation return handling are incomplete. Source presence does not guarantee mount isolation; device qualification remains pending.
- **ANDROID_ID Cache Poisoning**: Injects an anonymous `android.util.MemoryIntArray(1)` generation tracker into `android.provider.Settings$Secure.sNameValueCache` to spoof `ANDROID_ID` without generating detectable Binder RPC transactions to `SettingsProvider`.

---

## 4. Building from Source

### Prerequisites
- **Android NDK**: exactly r26b (`26.1.10909125`) (with LLVM toolchain targeting Android API 31+).
- **Host Compiler**: GCC 13+ or Clang 17+ supporting C++20.
- **Build Tools**: CMake 3.22+, GNU Make, Python 3.10+.

### Multi-ABI Cross-Compilation
```bash
# Build for all supported ARM ABIs (arm64-v8a and armeabi-v7a)
bash scripts/build_android.sh all

# Or build individual targets:
bash scripts/build_android.sh arm64-v8a
bash scripts/build_android.sh armeabi-v7a
```

### Packaging Flashable ZIP Module
```bash
python3 scripts/package_module.py
# Output archive: out/kaorioshelper-v1.0.0.zip
```

---

## 5. Verification & Testing

### Host Unit Tests
```bash
# Build and run native C++ unit tests
mkdir -p build && cd build
cmake .. && make -j$(nproc)
./test_native

# Run Python management tests
cd ..
PYTHONPATH=. python3 -m unittest discover -s tests -p "test_*.py"
```

### On-Device Diagnostic Test Fixture
The build pipeline cross-compiles native diagnostic test fixtures for Android:
- `out/kaorios_fixture` (arm64-v8a)
- `out/kaorios_fixture_arm32` (armeabi-v7a)

```bash
# Push fixture to device/emulator
adb push out/kaorios_fixture /data/local/tmp/
adb shell chmod 0755 /data/local/tmp/kaorios_fixture

# Run diagnostics with JSON output
adb shell su -c "/data/local/tmp/kaorios_fixture --json"
```

---

## 6. CLI Control Plane (`helperctl`)

KaoriosHelper includes a standalone native C++20 control plane binary (`helperctl`) and companion shell launcher (`helperctl.sh`):

```bash
# Query runtime engine status (supports --json for automation)
helperctl status
helperctl status --json

# Dynamically enable/disable subsystem engines
helperctl enable copg
helperctl disable tee

# Request reload; completion requires a matching successful runtime ACK
helperctl reload all
```

---

## 7. Security & Dual-Use Guidance

This project is developed for **authorized security research, defensive testing, CTF competitions, and device performance engineering**. Refuse requests for destructive techniques, denial-of-service, mass targeting, supply-chain compromise, or malicious evasion.

---

## 8. License

KaoriosHelper is licensed under the [GNU Affero General Public License v3.0 (AGPL-3.0-only)](LICENSE).  
Bundled third-party components remain licensed under their respective upstream open-source licenses:
- `ByteDance/ShadowHook`: Apache-2.0
- `topjohnwu/zygisk-module-sample`: Apache-2.0 / ISC
- `nlohmann/json`: MIT
- See [`docs/UPSTREAM_LICENSES.md`](docs/UPSTREAM_LICENSES.md) and [`upstreams.lock.json`](upstreams.lock.json) for full provenance and license tracking.
