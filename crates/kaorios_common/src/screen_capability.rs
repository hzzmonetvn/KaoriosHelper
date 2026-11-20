// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

/// Declaration only: no capture backend or process hooks are installed.
pub struct ScreenCaptureCapability;

impl ScreenCaptureCapability {
    pub const DEFAULT_ENABLED: bool = false;
    pub const SUPPORTED: bool = false;
    pub const BACKEND: &'static str = "unsupported";
    pub const STATUS: &'static str = "BLOCKED";
    pub const REASON: &'static str = "cooperative_test_app_backend_not_implemented";
}

#[cfg(test)]
mod tests {
    use super::ScreenCaptureCapability;

    #[test]
    fn screen_capability_remains_blocked() {
        assert!(!ScreenCaptureCapability::DEFAULT_ENABLED);
        assert!(!ScreenCaptureCapability::SUPPORTED);
        assert_eq!(ScreenCaptureCapability::BACKEND, "unsupported");
        assert_eq!(ScreenCaptureCapability::STATUS, "BLOCKED");
        assert_eq!(
            ScreenCaptureCapability::REASON,
            "cooperative_test_app_backend_not_implemented"
        );
    }
}
