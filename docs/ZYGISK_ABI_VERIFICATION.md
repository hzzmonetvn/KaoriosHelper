# KaoriosHelper Zygisk ABI Verification & Lifecycle Contract

## 1. Upstream ABI Standard Alignment

KaoriosHelper implements the official Zygisk ABI contract as established by `topjohnwu/zygisk-module-sample` and standardized across:
- **Magisk**: v24.0 through v27.0+
- **KernelSU**: Zygisk module support (v0.6.0+)
- **APatch**: APatch Zygisk implementation
- **Zygisk Next**: v1.0.0 through v1.2.0+

The header `third_party/zygisk/zygisk.hpp` is identical in ABI layout to upstream, ensuring binary compatibility with Zygisk loaders without symbol mangling or vtable discrepancies.

---

## 2. API Table Struct & Entrypoint Verification

The Zygisk loader identifies module entrypoints using two C-linkage symbols:
```c
extern "C" {
    void zygisk_module_entry(zygisk::internal::api_table *table, JNIEnv *env);
    void zygisk_companion_entry(int client_socket);
}
```

### 2.1 The `api_table` Layout
The internal ABI table struct (`zygisk::internal::api_table`) binds the following functional pointers:
1. `void *impl`: Pointer to loader-private context.
2. `bool (*registerModule)(api_table *, long, const char *)`: Registration handshake validating API version.
3. `void (*hookJniNativeMethods)(JNIEnv *, const char *, JNINativeMethod *, int)`: JNI trampoline hooking.
4. `void (*pltHookRegister)(dev_t, ino_t, const char *, void *, void **)`: Native PLT hooking dispatch.
5. `bool (*pltHookCommit)()`: Atomically commits registered PLT hooks.
6. `void (*setOption)(void *, zygisk::Option)`: Lifecycle option control:
   - `FORCE_DENYLIST_UNMOUNT`: Unmounts root overlays before app code runs.
   - `DLCLOSE_MODULE_LIBRARY`: Instructs Zygote to immediately `dlclose()` this module DSO.
7. `int (*connectCompanion)(void *)`: Establishes a UNIX domain socket connection to the root companion handler.
8. `uint32_t (*getFlags)(void *)`: Retrieves loader flags (`PROCESS_ON_DENYLIST`, `PROCESS_ROOT_IS_MAGISK_USER`).
9. `bool (*exemptFd)(int)`: Exempts file descriptors from Zygote fd-sanitizer checks.

### 2.2 Macro Registration
The module binds its entrypoints using the canonical macros in `zygisk/src/entry.cpp`:
```cpp
REGISTER_ZYGISK_MODULE(kaorios::KaoriosHelperModule)
REGISTER_ZYGISK_COMPANION(kaorios::companion_handler)
```
Compilation enforces GNU version script scoping (`scripts/zygisk_export.map`), guaranteeing that `zygisk_module_entry` and `zygisk_companion_entry` are the only exported dynamic symbols.

---

## 3. Zygote Process Lifecycle Dispatch

The unified dispatcher (`kaorios::KaoriosDispatcher`) routes Zygote lifecycle hooks across all constituent engines:

```
[Zygote Fork Process]
        │
        ├──> onLoad(zygisk::Api *api, JNIEnv *env)
        │       └─ Registers API pointer, initializes loggers
        │
   ┌────┴───────────────────────────┐
   │                                │
[App Process]                 [System Server]
   │                                │
   ├──> preAppSpecialize            ├──> preServerSpecialize
   │       │                                │
   │       ├─ Evaluate Target Apps          ├─ If HMA active:
   │       │   (PIF/TEE/COPG)               │    Load in-memory DEX
   │       │                                ├─ Else:
   │       └─ If Unmanaged:                 │    api->setOption(DLCLOSE)
   │            api->setOption(DLCLOSE)     │
   │                                        ├──> postServerSpecialize
   ├──> postAppSpecialize                   │       └─ Install PMS/Settings hooks
   │       │
   │       ├─ PIF: Libc shadowhook
   │       ├─ TEE: AIDL keystore isolation
   │       └─ COPG: Build & AID injection
```

### 3.1 Unmanaged App Lifecycle Safety (`DLCLOSE_MODULE_LIBRARY`)
To guarantee zero overhead and zero detection footprint in apps that are not explicitly targeted:
1. In `preAppSpecialize`, the dispatcher queries every engine (`shouldHandleApp(process_name, uid)`).
2. If **no engine** claims the current process, the dispatcher immediately invokes:
   ```cpp
   api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
   ```
3. Zygote unloads `arm64-v8a.so` from the process's memory space immediately after specialization, leaving zero memory remnants, open file descriptors, or hook artifacts in banking and everyday applications.

---

## 4. Root Companion Handler & Mount Namespace Confinement

### 4.1 Vulnerability of Global Mount Execution
In primitive module designs, companion handlers execute commands like `mount --bind /path /proc/cpuinfo` directly in the root daemon process. Because root daemons operate in the global/root mount namespace, bind-mounting `/proc/cpuinfo` modifies the file globally for the entire system, breaking core system diagnostics and alerting detection suites.

### 4.2 KaoriosHelper Target App Mount Confinement
KaoriosHelper enforces strict target app mount namespace confinement using Linux peer socket credentials (`SO_PEERCRED`) and namespace switching (`setns`):

1. **Peer PID Acquisition**:
   When the target app connects to the companion via `api->connectCompanion()`, the companion retrieves the target process PID:
   ```cpp
   struct ucred cr{};
   socklen_t len = sizeof(cr);
   getsockopt(client_socket, SOL_SOCKET, SO_PEERCRED, &cr, &len);
   pid_t target_pid = cr.pid;
   ```
2. **Namespace Switch**:
   The companion opens the target application's mount namespace `/proc/<target_pid>/ns/mnt` and executes `setns(target_ns, CLONE_NEWNS)`.
3. **Isolated Mount & Restore**:
   Inside the target application's private mount namespace, the companion executes:
   ```cpp
   umount2("/proc/cpuinfo", MNT_DETACH);
   mount(spoof_path, "/proc/cpuinfo", nullptr, MS_BIND, nullptr);
   ```
   After performing the mount, the companion restores its original root mount namespace before closing the client socket.
4. **Result**:
   The spoofed `/proc/cpuinfo` is visible **only** to the target application. All other system processes, system server, and root daemons see the authentic hardware `/proc/cpuinfo`.
