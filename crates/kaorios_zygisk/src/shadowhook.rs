// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project
#![allow(dead_code)]

use std::os::raw::{c_int, c_void};

pub const SHADOWHOOK_MODE_SHARED: c_int = 0;
pub const SHADOWHOOK_MODE_UNIQUE: c_int = 1;
pub const SHADOWHOOK_ERRNO_PENDING: c_int = 1;

#[cfg(all(
    target_os = "android",
    any(target_arch = "arm", target_arch = "aarch64")
))]
extern "C" {
    pub fn shadowhook_init(mode: c_int, debuggable: bool) -> c_int;
    pub fn shadowhook_get_errno() -> c_int;
    pub fn shadowhook_hook_sym_addr(
        sym_addr: *mut c_void,
        new_addr: *mut c_void,
        orig_addr: *mut *mut c_void,
    ) -> *mut c_void;
}

#[cfg(not(all(
    target_os = "android",
    any(target_arch = "arm", target_arch = "aarch64")
)))]
pub unsafe fn shadowhook_init(_mode: c_int, _debuggable: bool) -> c_int {
    0
}

#[cfg(not(all(
    target_os = "android",
    any(target_arch = "arm", target_arch = "aarch64")
)))]
pub unsafe fn shadowhook_get_errno() -> c_int {
    0
}

#[cfg(not(all(
    target_os = "android",
    any(target_arch = "arm", target_arch = "aarch64")
)))]
pub unsafe fn shadowhook_hook_sym_addr(
    _sym_addr: *mut c_void,
    _new_addr: *mut c_void,
    _orig_addr: *mut *mut c_void,
) -> *mut c_void {
    std::ptr::null_mut()
}
