# KaoriosHelper Architecture Specification

## 1. High-Level Vision & Design Principles

**KaoriosHelper** is a unified Root + Zygisk framework designed to serve as the Root Mode runtime backend for **Kaorios Toolbox**. It consolidates the distinct capabilities of four leading open-source root utilities:
1. **HMA Engine**: Advanced application visibility filtering, package manager filtering, installer source spoofing, and settings protection.
2. **PIF Engine**: Device profile emulation and GMS/DroidGuard system property virtualization.
3. **TEE Engine**: Hardware-backed keystore attestation simulation via KeyMint / Keystore2 interception.
4. **COPG Engine**: Per-app device profile spoofing, CPU information emulation, and gaming optimizations.

### 1.1 Architectural Axioms
1. **One Module Identity**: Distributed as a single installable root module (`kaorioshelper`).
2. **Unified Storage Contract**: All runtime configurations reside in `/data/adb/modules/kaorioshelper/data/` (`pif.json`, `keybox.xml`, `spoof.json`, `hma.json`).
3. **Decoupled Engines**: Engines operate as independent modular subsystems coordinated by a centralized runtime controller.
4. **Zero-Overhead Dispatcher**: A single unified Zygisk library (`libkaorioshelper.so`) selectively dispatches to engine hooks based on process identity and immediately requests `dlclose` in unmanaged processes.
5. **Fail-Closed Boot Safety**: No configuration error or hook crash is permitted to panic `system_server` or induce bootloops.

---

## 2. Directory Layout & Source Tree

```text
kaorioshelper/
├── core/
│   ├── config/             # ConfigManager: atomic file persistence, schema validation, migrations
│   ├── runtime/            # RuntimeManager: engine lifecycles, readiness tracking, health checks
│   ├── registry/           # HookRegistry: conflict-aware hook ownership enforcement
│   ├── status/             # StatusManager: precise multi-phase status tracking
│   ├── logging/            # DiagnosticLogger: structured logging with secret redaction
│   └── security/           # Permission validation, path traversal defense, symlink rejection
├── engines/
│   ├── hma/                # HMA-OSS integration (PMS & settings hooks for system_server)
│   ├── pif/                # Play Integrity Fix integration (GMS & DroidGuard virtualization)
│   ├── tee/                # TEE Simulator integration (KeyMint / Keystore2 attestation daemon)
│   └── copg/               # COPG integration (per-app Build & CPU spoofing)
├── zygisk/
│   ├── dispatcher/         # Central Zygisk entry point (KaoriosHelperModule)
│   └── native/             # Shared native hook utilities (ShadowHook, JNI helpers)
├── cli/
│   └── helperctl/          # Privileged command-line controller and Toolbox IPC bridge
├── module/
│   ├── module.prop         # Module identity metadata
│   ├── customize.sh        # Installation and prerequisite validation script
│   ├── post-fs-data.sh     # Early-boot preparation and verified boot property setup
│   ├── service.sh          # Post-boot daemon startup and background monitoring
│   ├── uninstall.sh        # Safe cleanup script
│   └── data/               # Canonical storage contract directory
├── docs/                   # Formal architectural specifications and audits
├── tests/                  # Automated host and unit test suite
├── upstreams.lock.json     # Pinned upstream commit lockfile
└── build.py / CMakeLists   # Reproducible build automation
```

---

## 3. Core Subsystems

### 3.1 ConfigManager
- **Contract Path**: `/data/adb/modules/kaorioshelper/data/`
  - `pif.json`: GMS/DroidGuard spoofing properties and advanced flags.
  - `keybox.xml`: Leaked or generated OEM Keybox containing RSA/ECDSA keys and certificates.
  - `spoof.json`: Per-app game and device spoofing profiles and CPU configurations.
  - `hma.json`: Package hiding rules, templates, and settings replacements.
- **Atomic Staging & Write Flow**:
  1. Acquire exclusive write lock (`flock` on `.lock` descriptor).
  2. Validate payload against formal versioned JSON/XML schemas.
  3. Reject path traversal attempts, directory symlinks, and oversized files.
  4. Write payload to temporary file on the same filesystem (`<file>.tmp.<pid>`).
  5. Enforce restrictive permissions (`0600`, owned by `root:root`, SELinux context `u:object_r:system_file:s0`).
  6. Call `fsync()` to flush buffers to non-volatile storage.
  7. Atomically rename temporary file to target (`rename(2)`).
  8. Increment configuration generation counter (`generation.txt`).
  9. Trigger runtime reload notification to affected engines.
- **Fail-Safe Rollback**:
  - Automatically maintains `.last_known_good` backups.
  - If a runtime error occurs during reload, the engine reverts to the last known good configuration without crashing.

### 3.2 RuntimeManager
- Manages the lifecycle states of all four engines:
  - `UNINITIALIZED` -> `INITIALIZING` -> `READY` -> `DEGRADED` -> `DISABLED`
- Provides independent enable/disable toggles for each engine (`tee.enabled`, `copg.enabled`, `hma.enabled`, `pif.enabled`).
- Orchestrates startup sequence across boot phases.

### 3.3 HookRegistry
- Maintains the authoritative mapping of system entry points to owner engines (see `docs/HOOK_OWNERSHIP.md`).
- Enforces strict process gating:
  - `system_server` -> HMA Engine only.
  - `com.google.android.gms.unstable` & `com.android.vending` -> PIF Engine only.
  - Configured user game packages -> COPG Engine only.
  - All other processes -> Module unloads cleanly (`DLCLOSE_MODULE_LIBRARY`).

### 3.4 StatusManager
- Exposes structured operational state across dual dimensions:
  1. **Persistence State**: `NOT_CONFIGURED` | `SAVED` | `INVALID_CONFIG`
  2. **Runtime Execution State**: `APPLIED` | `PENDING_RELOAD` | `REBOOT_REQUIRED` | `ENGINE_UNAVAILABLE` | `APPLY_FAILED`
- Eliminates false-positive reporting: an active config file on disk does not claim "applied" until the runtime engine confirms execution.

### 3.5 DiagnosticLogger
- Central structured logging channel (`/data/adb/modules/kaorioshelper/data/logs/runtime.log`).
- **Privacy & Security Protection**:
  - Automatically strips and redacts private key material, `<PrivateKey>`, `<Certificate>`, and authentication tokens.
  - Caps log file size with circular rotation to prevent storage exhaustion.

---

## 4. Unified Zygisk Dispatcher Architecture

Instead of running four separate Zygisk shared libraries—which compounds dlopen overhead, consumes memory in every app process, and creates hook racing—KaoriosHelper uses a single dispatcher library:

```text
                             Android Zygote Process
                                       │
                      ┌────────────────┴────────────────┐
                      ▼                                 ▼
             preAppSpecialize()                preServerSpecialize()
                      │                                 │
           [Process Inspection]                         ▼
                      │                         [system_server?]
         ┌────────────┼────────────┐                    │
         ▼            ▼            ▼             ┌──────┴──────┐
      [Is GMS?]  [In spoof.json?] [Other App]   ▼             ▼
         │            │            │          [YES]         [NO]
         ▼            ▼            ▼            │             │
     Dispatch:    Dispatch:      dlclose()   Dispatch:     dlclose()
     PIF Engine  COPG Engine     (Unload)    HMA Engine    (Unload)
```

### 4.1 Process Routing Logic
```cpp
void KaoriosHelperModule::preAppSpecialize(zygisk::AppSpecializeArgs* args) {
    std::string processName = getProcessName(args->nice_name);
    
    if (isGmsAttestationProcess(processName)) {
        if (runtimeManager.isPifEnabled()) {
            pifEngine.preAppSpecialize(args);
            return;
        }
    } else if (runtimeManager.isCopgEnabled() && copgEngine.hasTarget(processName)) {
        copgEngine.preAppSpecialize(args);
        return;
    }
    
    // Target is neither GMS nor a configured game/app: unload immediately
    api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
}

void KaoriosHelperModule::preServerSpecialize(zygisk::ServerSpecializeArgs* args) {
    if (runtimeManager.isHmaEnabled()) {
        hmaEngine.preServerSpecialize(args);
    } else {
        api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
}
```

---

## 5. Storage Contract: Kaorios Toolbox ↔ KaoriosHelper

All interaction between the Kaorios Toolbox frontend and KaoriosHelper occurs through the standardized directory:
`/data/adb/modules/kaorioshelper/data/`

| File | Format | Schema Owner | Purpose |
|---|---|---|---|
| `pif.json` | JSON v1 | PIF Engine | Device properties and GMS spoofing parameters |
| `keybox.xml` | XML | TEE Engine | Keybox keys and X.509 certificates for attestation |
| `spoof.json` | JSON v1 | COPG Engine | Per-app device profiles, Build overrides, and CPU profiles |
| `hma.json` | JSON v2 | HMA Engine | Application hiding rules, templates, and presets |
| `status.json` | JSON v1 | StatusManager | Real-time status reporting for all engines |
| `control.sock` | UNIX Domain | CLI / IPC | Local privileged command & control endpoint |

---

## 6. Dual Work Mode Compatibility

Kaorios Toolbox supports two operational architectures:
1. **ROM Integrated Mode**: Features patched directly into ROM system framework (`framework.jar`, `services.jar`). Managed via system settings and internal framework APIs.
2. **Root + Zygisk + Helper Mode**: Features provided dynamically without modifying system partitions. KaoriosHelper is the sole runtime provider for this mode.

KaoriosHelper preserves absolute independence:
- Does not modify or rely on ROM Integrated framework hooks.
- Detects if running on a Kaorios-patched ROM and avoids redundant hooks.
- Communicates with Toolbox via standard root IPC and atomic files.
