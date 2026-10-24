# Upstream Licenses & Compliance Analysis

## 1. Compliance Architecture Overview

KaoriosHelper integrates components derived from multiple open-source projects with varying copyleft and permissive licenses. To ensure complete legal compliance and freedom of software:
1. **Unified Module License**: The composite KaoriosHelper framework, custom dispatcher glue code, CLI tools, and new native libraries are licensed under **GNU Affero General Public License v3.0 (`AGPL-3.0-only`)**.
2. **License Compatibility Matrix**:
   - **AGPL-3.0 and GPL-3.0**: Under Section 13 of GPLv3 and AGPLv3, programs under GPLv3 may be linked and combined into a single work with code licensed under AGPLv3, provided the combined work is governed by AGPLv3 for the whole.
   - **Apache-2.0 and AGPL-3.0 / GPL-3.0**: Apache-2.0 code is fully compatible with GPLv3 and AGPLv3 (as confirmed by the Free Software Foundation and Apache Software Foundation), allowing Apache-2.0 source code to be included in an AGPLv3 derivative work while preserving the Apache-2.0 license notice and copyright in the source files.
   - **Permissive Libraries (MIT, ISC, BSD-2-Clause)**: Permissive code is compatible with all copyleft licenses. All original copyright banners and license notices are retained verbatim.
   - **OpenJDK Classpath Exception**: PanamaPort components derived from OpenJDK carry the GNU General Public License v2 with the Classpath Exception, permitting runtime linking without imposing GPLv2 terms on independent modules.
3. **Attribution & Notice Preservation**: All upstream copyright notices, authors, and NOTICE files are preserved in the source repository and embedded in binary release distributions.
4. **No Proprietary Blobs**: No closed-source binaries, trial/PRO licensing enforcement mechanisms, or components of dubious provenance are included.

---

## 2. Upstream Component Inventory & Licensing Details

### 2.1 HMA-OSS
- **Component**: HMA-OSS (Hide My Applist - Open Source)
- **Repository**: `https://github.com/frknkrc44/HMA-OSS.git`
- **Pinned Commit**: `1fe86ce2b4a6f21197e6e92d15e054d609b7d77c` (Tag `oss-173`)
- **SPDX License Identifier**: `AGPL-3.0-only`
- **Copyright**:
  - Copyright (C) 2021-2026 Furkan Karcıoğlu (`frknkrc44`)
  - Copyright (C) 2021-2026 HMA-OSS contributors
  - Copyright (C) 2020-2022 nullptr (Original HideMyApplist)
- **Source Disclosure Obligation**: Mandatory under AGPL-3.0. Complete source code of all modifications and build recipes must be provided.
- **Redistribution Requirements**:
  - Distribute under AGPL-3.0.
  - Retain all copyright notices, warranty disclaimers, and license text.
  - Prominently state modified files and date of modification.
- **Sub-dependencies within HMA-OSS**:
  - **AndroidVMTools**: `https://github.com/aerath-stuff/AndroidVMTools.git` (Commit `6fa2d5cbbf51c96fb81a9b5d96fdd74beac4aafe`). License: `MIT`.
  - **ZygoteLoader**: `https://github.com/aerath-stuff/ZygoteLoader.git` (Commit `2a2160fea2ed3bdbcb84fe48114718408731d9ad`). License: `MIT`.
  - **PanamaPort**: `https://github.com/aerath-stuff/PanamaPort.git` (Commit `c0d8fa431e9a747dd974b8f8c02695dc3ae38b6f`). License: `MIT` (Core/panama) & `GPL-2.0-with-classpath-exception` (OpenJDK components).
  - **Rikka Hidden API Compat**: `dev.rikka.hidden:compat:4.4.0`. License: `Apache-2.0`.
  - **R8Annotations**: `io.github.vova7878:R8Annotations:v1.0.1`. License: `MIT`.

### 2.2 Play Integrity Fork
- **Component**: PlayIntegrityFork (Fork of PlayIntegrityFix)
- **Repository**: `https://github.com/osm0sis/PlayIntegrityFork.git`
- **Pinned Commit**: `23ec56c2440c4fd1a5e893a23ef7861d6366a828` (Tag `v18`: `6d2307ff5037519982205e66bda1132d0d8cc4da`)
- **SPDX License Identifier**: `GPL-3.0-only`
- **Copyright**:
  - Copyright (C) 2023-2026 osm0sis
  - Copyright (C) 2023-2024 chiteroman
- **Source Disclosure Obligation**: Mandatory under GPL-3.0.
- **Redistribution Requirements**:
  - Distribute under GPL-3.0 or compatible AGPL-3.0 combined work.
  - Preserve original copyright and license header in `main.cpp`, `EntryPoint.java`, and shell scripts.
  - Document adaptations (integrating config into `/data/adb/modules/kaorioshelper/data/pif.json` and disabling conflicting `spoofProvider`).
- **Sub-dependencies within PIF**:
  - **ShadowHook**: `https://github.com/bytedance/shadowhook` by ByteDance Ltd. License: `Apache-2.0`.
  - **nlohmann/json**: `https://github.com/nlohmann/json` by Niels Lohmann. License: `MIT`.
  - **Zygisk API**: Magisk Zygisk API. License: `Apache-2.0` / `ISC`.

### 2.3 AlwaysStrong
- **Component**: AlwaysStrong
- **Repository**: `https://gitlab.com/Evokerr/AlwaysStrong.git`
- **Pinned Commit**: `95ccd2add35a2db2721b9e5111f13917f7dbac0c` (Tag `v1.0.5`: `72309a7c5117f28f44a1cd72b89ff1c5302208bf`)
- **SPDX License Identifier**: `GPL-3.0-only`
- **Copyright**:
  - Copyright (C) 2026 Evokerr
- **Source Disclosure Obligation**: Mandatory under GPL-3.0.
- **Redistribution Requirements**:
  - Distribute under GPL-3.0 or AGPL-3.0 combined work.
  - Preserve author attribution and copyright notices.
  - Ported concepts (inotify-based package watcher, Happy-Eyeballs fetcher, boot property coordination) must carry explicit attribution.

### 2.4 TEESimulator-RS
- **Component**: TEESimulator-RS
- **Repository**: `https://github.com/Enginex0/TEESimulator-RS.git`
- **Pinned Commit**: `6d241e56d6e8146cd67ed4ac3dadc9c632969549` (Tag `v6.0.1-307`)
- **SPDX License Identifier**: `GPL-3.0-only`
- **Copyright & Attributions**:
  - Copyright (C) 2024-2026 Enginex0
  - Includes derivative components and code from TrickyStore and TrickyStoreOSS (Copyright (C) beakthoven, JingMatrix, 5ec1cff).
  - Includes AOSP Keystore headers under `Apache-2.0`.
  - Includes Linux Kernel binder headers under `GPL-2.0 WITH Linux-syscall-note`.
- **Source Disclosure Obligation**: Mandatory under GPL-3.0.
- **Redistribution Requirements**:
  - Distribute under GPL-3.0 / AGPL-3.0.
  - Retain `NOTICE` file acknowledging TrickyStore, TrickyStoreOSS, and third-party components.

### 2.5 COPG
- **Component**: COPG (Historical open-source snapshot v5.1.1)
- **Repository**: `https://github.com/AlirezaParsi/COPG.git`
- **Pinned Commit**: `07c6482eaf83bf93697e88648491abf9053121c6`
- **SPDX License Identifier**: `Apache-2.0`
- **Copyright**:
  - Copyright (C) 2024-2026 AlirezaParsi (`Aliza`)
- **Source Disclosure & Redistribution Requirements**:
  - Apache-2.0 permits redistribution and integration into copyleft works (GPLv3 / AGPLv3).
  - Must retain the original `LICENSE` file.
  - Must include a `NOTICE` file indicating that the original code is Apache-2.0 and that modifications were made by KaoriosHelper.
  - Must preserve all original copyright banners in modified files (`src/spoof_module.cpp`, `src/unified_controller.cpp`).
- **Sub-dependencies within COPG (Commit 07c6482e)**:
  - **nlohmann/json**: Niels Lohmann, Evan Nemerson. License: `MIT`.
  - **zygisk.hpp**: Magisk Zygisk API. License: `ISC`.
  - **atexit.cpp**: Android Open Source Project. License: `BSD-2-Clause`.

---

## 3. License Compatibility & Union Matrix

| Component | Upstream License | Compatible with AGPL-3.0? | Combination Mechanism | Governing License of Combination |
|---|---|---|---|---|
| **KaoriosHelper Core** | AGPL-3.0-only | **Yes** (Native) | Project Base | AGPL-3.0-only |
| **HMA-OSS** | AGPL-3.0-only | **Yes** (Identical) | In-process execution in `system_server` | AGPL-3.0-only |
| **Play Integrity Fork** | GPL-3.0-only | **Yes** (Section 13) | Zygisk companion + DEX payload | AGPL-3.0-only |
| **AlwaysStrong Components** | GPL-3.0-only | **Yes** (Section 13) | Native daemons & scripts | AGPL-3.0-only |
| **TEESimulator-RS** | GPL-3.0-only | **Yes** (Section 13) | Daemon supervisor & keystore injector | AGPL-3.0-only |
| **COPG (Commit 07c6482e)** | Apache-2.0 | **Yes** (GPLv3 compatible) | Merged native Zygisk companion & controller | AGPL-3.0-only |
| **AndroidVMTools** | MIT | **Yes** (Permissive) | Embedded library | AGPL-3.0-only |
| **ShadowHook** | Apache-2.0 | **Yes** (Permissive) | Linked static/shared library | AGPL-3.0-only |
| **ZygoteLoader** | MIT | **Yes** (Permissive) | Injection stub | AGPL-3.0-only |
| **PanamaPort** | MIT + Classpath Exception | **Yes** | Native reflection runtime | AGPL-3.0-only |

---

## 4. Required Notices & Attribution Deliverables

In compliance with Apache-2.0 Section 4 and GPLv3/AGPLv3 Section 5:
1. Every modified source file carries an explicit header:
   ```cpp
   // SPDX-License-Identifier: AGPL-3.0-only
   // Originally part of [Upstream Component Name]
   // Original Copyright (C) [Year] [Original Author]
   // Modified by HzzMonet / Kaorios Project on [Date] for KaoriosHelper
   ```
2. The root of the repository provides:
   - `LICENSE` (Full text of GNU AGPL-3.0)
   - `NOTICE` (Detailed attribution to all upstream authors and libraries)
   - `docs/UPSTREAM_LICENSES.md` (This document)
3. Packaged release ZIPs contain a dedicated `licenses/` directory packaging the license text of all bundled components.
