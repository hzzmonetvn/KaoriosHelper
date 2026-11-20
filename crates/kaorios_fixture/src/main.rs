// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project
// KaoriosHelper Android On-Device Diagnostic Test Fixture

use serde::Serialize;
use serde_json::json;
use std::ffi::{CStr, CString};
use std::fs;
use std::os::raw::{c_char, c_int};
use std::process::Command;

#[derive(Serialize)]
struct TestResult {
    name: String,
    category: String,
    passed: bool,
    expected: String,
    actual: String,
    details: String,
}

#[cfg(target_os = "android")]
extern "C" {
    fn __system_property_get(name: *const c_char, value: *mut c_char) -> c_int;
}

#[cfg(not(target_os = "android"))]
unsafe fn __system_property_get(_name: *const c_char, _value: *mut c_char) -> c_int {
    0
}

fn get_prop(key: &str) -> String {
    let Ok(c_key) = CString::new(key) else {
        return "<NOT_FOUND>".to_string();
    };
    let mut buf = [0u8; 128];
    let len = unsafe { __system_property_get(c_key.as_ptr(), buf.as_mut_ptr().cast()) };
    if len > 0 {
        unsafe { CStr::from_ptr(buf.as_ptr().cast()) }
            .to_string_lossy()
            .into_owned()
    } else {
        "<NOT_FOUND>".to_string()
    }
}

fn main() {
    let mut results = Vec::new();
    let mut record = |name: &str, category: &str, passed: bool, expected: &str, actual: &str, details: &str| {
        results.push(TestResult {
            name: name.to_string(),
            category: category.to_string(),
            passed,
            expected: expected.to_string(),
            actual: actual.to_string(),
            details: details.to_string(),
        });
    };

    let fp = get_prop("ro.build.fingerprint");
    record(
        "prop.ro.build.fingerprint",
        "PIF",
        !fp.is_empty() && fp != "<NOT_FOUND>",
        "Non-empty spoofed or OEM fingerprint",
        &fp,
        "",
    );

    let bootstate = get_prop("ro.boot.verifiedbootstate");
    record(
        "prop.ro.boot.verifiedbootstate",
        "PIF",
        bootstate == "green",
        "green",
        &bootstate,
        "Play Integrity requires green for locked bootloader illusion",
    );

    let flash_locked = get_prop("ro.boot.flash.locked");
    record(
        "prop.ro.boot.flash.locked",
        "PIF",
        flash_locked == "1",
        "1",
        &flash_locked,
        "",
    );

    let sym = CString::new("__system_property_read_callback").unwrap();
    let p_read_cb = unsafe { libc::dlsym(libc::RTLD_DEFAULT, sym.as_ptr()) };
    record(
        "libc.__system_property_read_callback",
        "PIF_HOOK",
        !p_read_cb.is_null(),
        "Symbol present in libc",
        if p_read_cb.is_null() { "Missing" } else { "Found" },
        "",
    );

    let cpuinfo = fs::read_to_string("/proc/cpuinfo");
    let openable = cpuinfo.is_ok();
    let first_line = cpuinfo
        .as_ref()
        .ok()
        .and_then(|s| s.lines().next())
        .unwrap_or("")
        .to_string();
    record(
        "proc.cpuinfo.readable",
        "COPG",
        openable,
        "true",
        if openable { "true" } else { "false" },
        "",
    );
    let has_proc = first_line.contains("processor")
        || first_line.contains("Processor")
        || first_line.contains("Hardware");
    record(
        "proc.cpuinfo.structure",
        "COPG",
        has_proc,
        "Valid cpuinfo header",
        &first_line,
        "",
    );

    let aid = Command::new("/system/bin/settings")
        .args(["get", "secure", "android_id"])
        .output()
        .ok()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .unwrap_or_default();
    record(
        "settings.secure.android_id",
        "COPG",
        !aid.is_empty() && aid != "null",
        "16-char hex string",
        &aid,
        "",
    );

    let passed_count = results.iter().filter(|r| r.passed).count();
    let out = json!({
        "fixture": "kaorios_fixture",
        "total": results.len(),
        "passed": passed_count,
        "results": results,
    });
    println!("{}", serde_json::to_string_pretty(&out).unwrap());
}
