# Build Requirements & Environment Specification

## 1. Required Build Toolchains & Host Environment

Building KaoriosHelper reproducibly from source requires the following toolchain components:

| Tool | Minimum Version | Recommended Version | Purpose |
|---|---|---|---|
| **Host Operating System** | Linux x86_64 (glibc 2.35+) | Ubuntu 24.04 LTS / Arch Linux / CachyOS | Build host environment |
| **Android NDK** | NDK r26b | NDK r27c / r29 | Compiling C/C++ native Zygisk companion & daemons |
| **CMake** | 3.22.1 | 3.28+ | Native build system |
| **Java Development Kit (JDK)** | OpenJDK 17 | Eclipse Temurin 17 / Zulu 17 | Compiling Java/Kotlin DEX payloads (HMA & PIF) |
| **Gradle** | 8.10 | 8.13 | Java/Kotlin project orchestration |
| **Android SDK Build-Tools** | 34.0.0 | 35.0.0 (`d8`, `zipalign`, `apksigner`) | Compiling DEX bytecode from compiled class files |
| **Rust & Cargo** | 1.78.0 | 1.82+ | Compiling native watcher daemon (`aswatcher`) and TEE tools |
| **Python** | 3.10 | 3.12+ | Automation scripts, packaging, and host regression testing |
| **Zip & Unzip** | Info-ZIP 3.0 | Info-ZIP 3.0 | Module archive packaging |

---

## 2. Target Architectures & Toolchain Flags

### 2.1 Target ABIs
1. **`arm64-v8a`** (Primary focus: modern Android 12–17 devices)
2. **`armeabi-v7a`** (32-bit legacy ARM processes)
*(Note: x86 and x86_64 support has been removed; KaoriosHelper is built exclusively for ARM devices)*

### 2.2 Critical Native Compiler Flags
```cmake
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization and size reduction
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Os -flto -fPIE -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden -fvisibility-inlines-hidden")

# Security hardening & memory safety
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fstack-protector-strong -D_FORTIFY_SOURCE=2")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,relro -Wl,-z,now")
```

---

## 3. Subsystem Build Pipelines

### 3.1 Native Zygisk Companion (`libkaorioshelper.so`)
- **Source**: `zygisk/dispatcher/` + `engines/copg/` + `engines/pif/`
- **Output**:
  - `module/zygisk/arm64-v8a.so`
  - `module/zygisk/armeabi-v7a.so`
- **Link Dependencies**: `libc++_static.a`, `liblog.so`, `libshadowhook.so` (static or symbol-hidden).

### 3.2 HMA Engine DEX (`hma_classes.dex`)
- **Source**: `engines/hma/` + `common/` + `external/AndroidVMTools`
- **Pipeline**:
  1. Compile Kotlin & Java sources using Gradle (`compileReleaseJavaWithJavac`, `compileReleaseKotlin`).
  2. Run R8 minification with `@DoNotShrink` and `@DoNotObfuscate` rules.
  3. Desugar and translate to Dalvik bytecode using `d8 --min-api 29`.
  4. Output: `module/hma_classes.dex`.

### 3.3 PIF Engine DEX (`pif_classes.dex`)
- **Source**: `engines/pif/java/` (`EntryPoint.java`, `CustomProvider.java`)
- **Pipeline**:
  1. Compile with `javac -target 1.8 -source 1.8`.
  2. Translate with `d8 --min-api 26`.
  3. Output: `module/pif_classes.dex`.

### 3.4 TEE Attestation Daemon & Injector
- **Source**: `engines/tee/`
- **Output**:
  - `module/tee/daemon`
  - `module/tee/supervisor`
  - `module/tee/inject`
  - `module/tee/libTEESimulator.so`
  - `module/tee/libcertgen.so`
  - `module/tee/tee_classes.dex`

### 3.5 Control CLI (`helperctl`)
- **Source**: `cli/helperctl/`
- **Implementation**: POSIX `/system/bin/sh` script with atomic helpers and optional native companion C binary for cryptographic verification.
- **Output**: `module/helperctl.sh` (symlinked or executed as `helperctl`).

---

## 4. Module Assembly & Packaging

The packaging script (`tools/package_module.py`):
1. Verifies that all target binaries match the expected ELF architecture (`arm64-v8a`, `ELF 64-bit LSB shared object`).
2. Validates that no private keys, `.jks`, `.keystore`, or leaked `keybox.xml` files are included in the build output.
3. Sets standard POSIX permissions:
   - Scripts (`customize.sh`, `service.sh`, `post-fs-data.sh`, `helperctl.sh`): `0755`
   - Binaries (`daemon`, `supervisor`, `inject`, `.so`): `0755`
   - Metadata (`module.prop`, `README.md`, licenses): `0644`
4. Generates cryptographic checksums (`SHA256SUMS.txt`).
5. Assembles the final zip: `out/KaoriosHelper-<version>.zip`.

---

## 5. Host Test Requirements

Automated verification runs on any standard Linux host with Python 3.10+:
```sh
python3 -m unittest discover -s tests -v
```
No connected Android device or root environment is required for host unit tests, schema validation, or packaging verification.
