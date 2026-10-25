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

## 2. Subsystems & Maturity Matrix

Under strict engineering truthfulness protocols, KaoriosHelper is classified as **ALPHA (Developer / Lab Test Build)**. Production readiness cannot be claimed until physical device field testing is completed (`DEVICE_NOT_TESTED`) and mandatory runtime components are fully resolved.

| Subsystem | Upstream Ancestor | Maturity | Hook & Runtime Mechanism | Device Status |
|---|---|---|---|---|
| **Zygisk Core** | `topjohnwu/zygisk-module-sample` | **PRODUCTION (100%)** | Official Zygisk ABI v4, `dlclose` unmanaged, `SO_PEERCRED` namespace isolation | `DEVICE_NOT_TESTED` |
| **Control Plane** | Native C++ Rewrite | **PRODUCTION (100%)** | Pure C++20 CLI (`helperctl`), multi-ABI, atomic file staging, `flock(LOCK_EX)` | `DEVICE_NOT_TESTED` |
| **PIF Engine** | `osm0sis/PlayIntegrityFork` | **BETA (95%)** | ShadowHook inline hook on Bionic libc `__system_property_read_callback` | `DEVICE_NOT_TESTED` |
| **COPG Engine** | `07c6482e` (COPG) | **BETA (90%)** | Peer `setns` mount (`MS_SLAVE`), client-side `ANDROID_ID` cache poisoning | `DEVICE_NOT_TESTED` |
| **TEE Engine** | `TrickyStore` / `AlwaysStrong` | **ALPHA (50%)** | Keybox XML parser active; Keystore2 ptrace daemon external; disabled by default | `DEVICE_NOT_TESTED` |
| **HMA Engine** | `frknkrc44/HMA-OSS` | **ALPHA (40%)** | Native `InMemoryDexClassLoader` scaffolding; DEX unbundled (gated on JDK/SDK build) | `DEVICE_NOT_TESTED` |

---

## 3. Honest Runtime Blockers & Technical Analysis

### 3.1 HMA Engine: Platform Compilation Blocker
- **Architecture**: Injects into `system_server` (UID 1000) during `preServerSpecialize`. Employs `dalvik.system.InMemoryDexClassLoader` via Direct ByteBuffers targeting `org.frknkrc44.hma_oss.zygote.ZygoteEntry`.
- **Platform Blocker**: Compiling the upstream HMA Java/Kotlin bytecode requires a full OpenJDK (`javac`) and Android SDK 34 platform build-tools (`d8`). Headless build environments with JRE-only runtimes fail Gradle build with:
  ```
  org.gradle.api.internal.catalog.GeneratedClassCompilationException: No Java compiler found, please ensure you are running Gradle with a JDK
  ```
- **Runtime Safety**: In the absence of an externally provided `classes.dex`, the native C++ engine logs a non-fatal warning and safely skips bytecode injection, ensuring `system_server` boots reliably without crashes or bootloops.

### 3.2 TEE Engine: Keystore2 System Daemon Isolation
- **Architecture**: Validates Google Keybox XML schemas, ECDSA/RSA declarations, and certificate chains.
- **Daemon Isolation**: On Android 12+, `/system/bin/keystore2` is a native Rust daemon spawned directly by `init.rc` (UID 1017:1017, `u:r:keystore:s0`), completely outside Zygote.
- **Consequence for Zygisk**: Pure Zygisk modules cannot intercept `IKeyMintDevice::attestKey` inside `keystore2` without an out-of-process root daemon attaching via `ptrace(PTRACE_ATTACH)`.
- **Safe Default**: TEE is **disabled by default** (`enabled: false`) to safeguard Android lockscreen PIN decryption, biometric enrollment, and hardware key generation. XML parsing is not claimed as genuine hardware attestation.

### 3.3 COPG Engine: Mount Propagation Hardening
- **Namespace Virtualization**: Uses `getsockopt(SOL_SOCKET, SO_PEERCRED)` to obtain the client PID and switches into the target mount namespace via `setns(open("/proc/<pid>/ns/mnt"), CLONE_NEWNS)`.
- **Propagation Hardening**: Enforces `mount(nullptr, "/proc", nullptr, MS_SLAVE | MS_REC, nullptr)` before bind-mounting `/proc/cpuinfo`. This guarantees the mount **never leaks** into parent or peer namespaces.
- **ANDROID_ID Cache Poisoning**: Injects an anonymous `android.util.MemoryIntArray(1)` generation tracker into `android.provider.Settings$Secure.sNameValueCache` to spoof `ANDROID_ID` without generating detectable Binder RPC transactions to `SettingsProvider`.

---

## 4. Building from Source

### Prerequisites
- **Android NDK**: r26b or newer (with LLVM toolchain targeting Android API 31+).
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
# Build and run native C++ unit tests (27 tests)
mkdir -p build && cd build
cmake .. && make -j$(nproc)
./test_native

# Run Python management tests (29 tests)
cd ..
python3 -m unittest discover -s tests -p "test_*.py"
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

# Trigger atomic configuration reload
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
