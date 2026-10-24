# KaoriosHelper TEE Subsystem Runtime Evidence & Analysis

## 1. Upstream TEE & Hardware Attestation Architecture

Android 8.0+ introduced hardware-backed key attestation via Trusted Execution Environments (TEE) or StrongBox KeyMint hardware chips. When an application requests an attested key pair via `KeyStore.getInstance()`, the system:
1. Dispatches an IPC transaction to the system keystore daemon.
2. Keystore delegates to the KeyMint HAL (`android.hardware.security.keymint.IKeyMintDevice::attestKey`).
3. KeyMint generates a cryptographic key pair inside the secure enclave.
4. KeyMint synthesizes an X.509 certificate whose extension (OID `1.3.6.1.4.1.11129.2.1.17`) encodes the device's boot state, verified boot key, OS version, security patch level, and attestation challenge.
5. KeyMint signs the certificate using a private key burned into the hardware at manufacture time, chained back to Google's Hardware Attestation Root CA.

Upstream simulators (`AlwaysStrong`, `TEESimulator-RS`, `TrickyStore`) intercept this process by:
- Injecting into the keystore process or hooking `libbinder.so`.
- Reading an OEM-extracted `keybox.xml` containing valid private keys and certificates.
- Synthesizing the ASN.1 Key Description extension in software.
- Signing the leaf certificate using the Keybox private key, returning a valid certificate chain back to Google Play Services.

---

## 2. Keystore2 System Daemon Isolation vs. Zygote Architecture

### Critical Architecture Insight:
On Android 12 and above, the Android Keystore system was rewritten in Rust (`/system/bin/keystore2`). 
- **Process Model**: `/system/bin/keystore2` is spawned directly by `init` during early boot (`init.rc`).
- **Privilege & SELinux**: Runs as UID `1017:1017` (`keystore:keystore`) under SELinux domain `u:r:keystore:s0`.
- **Relationship to Zygote**: Keystore2 is **NOT** a child of Zygote.
- **Consequence for Zygisk**: Zygisk modules inject **strictly** into Zygote, `system_server`, and apps forked from Zygote. A Zygisk module cannot intercept `IKeyMintDevice` transactions inside `keystore2` without an external root daemon attaching to `keystore2` via `ptrace(PTRACE_ATTACH)` or modifying the dynamic linker preload.
- **Upstream Operation (`TEESimulator-RS` / `TrickyStore`)**:
  Upstream solutions deploy a standalone out-of-process root daemon (e.g. `service.sh` launching a native daemon) that locates the PID of `/system/bin/keystore2`, attaches via `ptrace`, injects `libtee.so` into the keystore process memory space, and hooks `android.hardware.security.keymint.IKeyMintDevice::attestKey`.

### Cryptographic & Integrity Integrity Notice:
- **XML Parsing is NOT KeyMint Interception**: Parsing `<KeyboxContent>` from `keybox.xml` does not constitute key attestation interception.
- **Software Attestation is NOT Hardware Attestation**: Emulated certificates signed by leaked OEM root keys cannot provide genuine hardware-backed security guarantees; they simulate the attestation response for verification clients.
- **Safe Default State**: To protect device key generation, lockscreen authentication, and banking keystore integrity, TEE is **disabled by default** (`enabled: false`).

---

## 3. KaoriosHelper Implementation (`engines/tee/tee_engine.cpp`)

KaoriosHelper implements the configuration, Keybox parser, and validation layer:

### 3.1 Keybox Validation Logic
In `TeeEngine::hasValidKeybox()`:
- Verifies existence and non-zero size of `/data/adb/modules/kaorioshelper/data/keybox.xml`.
- Validates the XML document structure against the Google Keybox schema:
  - Requires root tag `<KeyboxContent>` or `<AndroidAttestation>`.
  - Verifies presence of `<Key algorithm="ecdsa">` and `<Key algorithm="rsa">` nodes.
  - Verifies `<PrivateKey>` and `<Certificate>` blocks containing valid Base64 / PEM strings.
- If the file is missing or contains invalid certificates, `hasValidKeybox()` returns `false`, preventing malformed key injection.

### 3.2 Keystore Key Generation Safety Guard
Attestation hooking carries catastrophic risk if misconfigured: if an interceptor returns forged or broken certificates to everyday apps or the Android lockscreen, users can be permanently locked out of their devices (lockscreen PIN decryption failure) or banking apps can fail to generate Keystore keys.

To prevent this:
1. **Target Confinement**:
   `TeeEngine::shouldHandleApp` strictly rejects:
   - System Server (UID 1000).
   - Keystore daemon (UID 1017).
   - Everyday user apps (banking, DRM, streaming).
   - Only `com.google.android.gms` and `com.google.android.gms.unstable` are permitted.
2. **Mutual Exclusion with PIF**:
   When TEE is enabled with a valid Keybox, PIF's `spoofProvider` is forcefully overridden to `0` to prevent Java-layer `CustomProvider` from corrupting KeyStore queries.

---

## 4. Subsystem Status & Classification

| Dimension | Specification | Actual State in KaoriosHelper |
|---|---|---|
| Keybox XML Parser | Schema, ECDSA/RSA, Certificate validation | **VERIFIED IMPLEMENTED** |
| Target App Confinement | Strictly GMS; blocks Keystore daemon/banking | **VERIFIED IMPLEMENTED** |
| Mutual Exclusion | Disables PIF `spoofProvider` when TEE active | **VERIFIED IMPLEMENTED** |
| AIDL Binder Hooking | Native IPC interception in `keystore2` | **GATED (Requires root ptrace daemon)** |
| Cryptographic Signing | Leaf certificate ASN.1 synthesis | **EXTERNAL (Upstream ptrace integration)** |
| Safe Default State | Disabled by default to protect keygen | **VERIFIED (`enabled: false`)** |
| Host Test Verification | 6/6 Unit Tests Passed | **PASSED** |
| Physical Hardware Verification | Tested on live Android 12–15 device | **`DEVICE_NOT_TESTED`** |

### Status Classification: **ALPHA / KEYBOX_PARSER_ACTIVE (Gated on Root Ptrace Integration)**
KaoriosHelper contains an authentic, robust Keybox parser and safety controller. Because `/system/bin/keystore2` is an isolated native daemon outside Zygote, hardware KeyMint attestation spoofing requires an out-of-process root ptrace daemon. TEE remains disabled by default.
