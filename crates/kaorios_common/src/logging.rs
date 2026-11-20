// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use std::ffi::CString;

#[cfg(target_os = "android")]
mod android_log {
    use std::os::raw::{c_char, c_int};

    pub const ANDROID_LOG_DEBUG: c_int = 3;
    pub const ANDROID_LOG_INFO: c_int = 4;
    pub const ANDROID_LOG_WARN: c_int = 5;
    pub const ANDROID_LOG_ERROR: c_int = 6;

    extern "C" {
        pub fn __android_log_write(prio: c_int, tag: *const c_char, text: *const c_char) -> c_int;
    }
}

pub fn log_msg(level: &str, msg: &str) {
    #[cfg(target_os = "android")]
    {
        let prio = match level {
            "DEBUG" => android_log::ANDROID_LOG_DEBUG,
            "INFO" => android_log::ANDROID_LOG_INFO,
            "WARN" => android_log::ANDROID_LOG_WARN,
            _ => android_log::ANDROID_LOG_ERROR,
        };
        let c_tag = CString::new("KaoriosHelper").unwrap_or_default();
        let c_msg = CString::new(msg.replace('\0', "")).unwrap_or_default();
        unsafe {
            android_log::__android_log_write(prio, c_tag.as_ptr(), c_msg.as_ptr());
        }
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = CString::new("");
        if level == "ERROR" {
            eprintln!("[{level}] [KaoriosHelper] {msg}");
        } else {
            println!("[{level}] [KaoriosHelper] {msg}");
        }
    }
}

#[macro_export]
macro_rules! log_debug {
    ($($arg:tt)*) => {
        $crate::logging::log_msg("DEBUG", &format!($($arg)*))
    };
}

#[macro_export]
macro_rules! log_info {
    ($($arg:tt)*) => {
        $crate::logging::log_msg("INFO", &format!($($arg)*))
    };
}

#[macro_export]
macro_rules! log_warn {
    ($($arg:tt)*) => {
        $crate::logging::log_msg("WARN", &format!($($arg)*))
    };
}

#[macro_export]
macro_rules! log_error {
    ($($arg:tt)*) => {
        $crate::logging::log_msg("ERROR", &format!($($arg)*))
    };
}
