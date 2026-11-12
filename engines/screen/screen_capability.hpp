/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

#pragma once

namespace kaorios {

// Declaration only: no capture backend or process hooks are installed.
struct ScreenCaptureCapability {
    static constexpr bool default_enabled = false;
    static constexpr bool supported = false;
    static constexpr const char* backend = "unsupported";
    static constexpr const char* status = "BLOCKED";
    static constexpr const char* reason = "cooperative_test_app_backend_not_implemented";
};

} // namespace kaorios
