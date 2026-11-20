// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project
//
// Runtime Acknowledgement Protocol
// Secure, versioned IPC protocol for tracking runtime hook specialization and observation.

use crate::config_identity::config_sha256;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::HashSet;
use std::ffi::CString;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[repr(i32)]
pub enum AckEventType {
    ProcessLoaded = 1,
    ConfigLoaded = 2,
    HookRegistered = 3,
    HookInitialized = 4,
    HookObserved = 5,
    ApplyFailed = 6,
}

impl AckEventType {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::ProcessLoaded => "PROCESS_LOADED",
            Self::ConfigLoaded => "CONFIG_LOADED",
            Self::HookRegistered => "HOOK_REGISTERED",
            Self::HookInitialized => "HOOK_INITIALIZED",
            Self::HookObserved => "HOOK_OBSERVED",
            Self::ApplyFailed => "APPLY_FAILED",
        }
    }

    pub fn from_str_strict(s: &str) -> Option<Self> {
        match s {
            "PROCESS_LOADED" => Some(Self::ProcessLoaded),
            "CONFIG_LOADED" => Some(Self::ConfigLoaded),
            "HOOK_REGISTERED" => Some(Self::HookRegistered),
            "HOOK_INITIALIZED" => Some(Self::HookInitialized),
            "HOOK_OBSERVED" => Some(Self::HookObserved),
            "APPLY_FAILED" => Some(Self::ApplyFailed),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeAck {
    pub schema_version: i32,
    pub protocol_version: i32,
    pub engine: String,
    pub process_name: String,
    pub pid: libc::pid_t,
    pub uid: libc::uid_t,
    pub event_type: String,
    pub config_generation: u64,
    pub init_result: i32,
    pub error_code: i32,
    pub timestamp: u64,
    pub detail: String,
    pub boot_id: String,
    pub process_start_ticks: u64,
    pub config_hash: String,
    pub request_id: String,
}

impl Default for RuntimeAck {
    fn default() -> Self {
        Self {
            schema_version: 1,
            protocol_version: 1,
            engine: String::new(),
            process_name: String::new(),
            pid: 0,
            uid: 0,
            event_type: AckEventType::ProcessLoaded.as_str().to_string(),
            config_generation: 0,
            init_result: 0,
            error_code: 0,
            timestamp: 0,
            detail: String::new(),
            boot_id: String::new(),
            process_start_ticks: 0,
            config_hash: String::new(),
            request_id: String::new(),
        }
    }
}

impl RuntimeAck {
    pub fn event(&self) -> Option<AckEventType> {
        AckEventType::from_str_strict(&self.event_type)
    }

    pub fn set_event(&mut self, ev: AckEventType) {
        self.event_type = ev.as_str().to_string();
    }

    pub fn to_json_pretty(&self) -> String {
        serde_json::to_string_pretty(self).unwrap_or_default()
    }

    pub fn to_json_compact(&self) -> String {
        serde_json::to_string(self).unwrap_or_default()
    }
}

/// Validates JSON text has nesting depth <= 16 and no duplicate keys in any object.
fn check_json_depth_and_no_dup_keys(bytes: &str) -> bool {
    let b = bytes.as_bytes();
    let mut idx = 0usize;
    let len = b.len();

    fn skip_ws(b: &[u8], idx: &mut usize) {
        while *idx < b.len() && matches!(b[*idx], b' ' | b'\t' | b'\r' | b'\n') {
            *idx += 1;
        }
    }

    fn parse_string(b: &[u8], idx: &mut usize) -> Option<String> {
        if *idx >= b.len() || b[*idx] != b'"' {
            return None;
        }
        *idx += 1;
        let start = *idx;
        let mut has_escape = false;
        while *idx < b.len() {
            match b[*idx] {
                b'\\' => {
                    has_escape = true;
                    *idx += 1;
                    if *idx >= b.len() {
                        return None;
                    }
                    *idx += 1;
                }
                b'"' => {
                    let raw = std::str::from_utf8(&b[start - 1..=*idx]).ok()?;
                    *idx += 1;
                    if !has_escape {
                        return Some(raw[1..raw.len() - 1].to_string());
                    }
                    return serde_json::from_str::<String>(raw).ok();
                }
                0..=0x1f => return None,
                _ => *idx += 1,
            }
        }
        None
    }

    fn parse_value(b: &[u8], idx: &mut usize, depth: usize) -> bool {
        if depth > 16 {
            return false;
        }
        skip_ws(b, idx);
        if *idx >= b.len() {
            return false;
        }
        match b[*idx] {
            b'{' => {
                *idx += 1;
                skip_ws(b, idx);
                if *idx < b.len() && b[*idx] == b'}' {
                    *idx += 1;
                    return true;
                }
                let mut keys = HashSet::new();
                loop {
                    skip_ws(b, idx);
                    let Some(k) = parse_string(b, idx) else {
                        return false;
                    };
                    if !keys.insert(k) {
                        return false;
                    }
                    skip_ws(b, idx);
                    if *idx >= b.len() || b[*idx] != b':' {
                        return false;
                    }
                    *idx += 1;
                    if !parse_value(b, idx, depth + 1) {
                        return false;
                    }
                    skip_ws(b, idx);
                    if *idx >= b.len() {
                        return false;
                    }
                    if b[*idx] == b'}' {
                        *idx += 1;
                        return true;
                    }
                    if b[*idx] != b',' {
                        return false;
                    }
                    *idx += 1;
                }
            }
            b'[' => {
                *idx += 1;
                skip_ws(b, idx);
                if *idx < b.len() && b[*idx] == b']' {
                    *idx += 1;
                    return true;
                }
                loop {
                    if !parse_value(b, idx, depth + 1) {
                        return false;
                    }
                    skip_ws(b, idx);
                    if *idx >= b.len() {
                        return false;
                    }
                    if b[*idx] == b']' {
                        *idx += 1;
                        return true;
                    }
                    if b[*idx] != b',' {
                        return false;
                    }
                    *idx += 1;
                }
            }
            b'"' => parse_string(b, idx).is_some(),
            b't' => {
                if b[*idx..].starts_with(b"true") {
                    *idx += 4;
                    true
                } else {
                    false
                }
            }
            b'f' => {
                if b[*idx..].starts_with(b"false") {
                    *idx += 5;
                    true
                } else {
                    false
                }
            }
            b'n' => {
                if b[*idx..].starts_with(b"null") {
                    *idx += 4;
                    true
                } else {
                    false
                }
            }
            b'-' | b'0'..=b'9' => {
                let start = *idx;
                if b[*idx] == b'-' {
                    *idx += 1;
                }
                while *idx < b.len() && b[*idx].is_ascii_digit() {
                    *idx += 1;
                }
                if *idx < b.len() && b[*idx] == b'.' {
                    *idx += 1;
                    while *idx < b.len() && b[*idx].is_ascii_digit() {
                        *idx += 1;
                    }
                }
                if *idx < b.len() && matches!(b[*idx], b'e' | b'E') {
                    *idx += 1;
                    if *idx < b.len() && matches!(b[*idx], b'+' | b'-') {
                        *idx += 1;
                    }
                    while *idx < b.len() && b[*idx].is_ascii_digit() {
                        *idx += 1;
                    }
                }
                *idx > start
            }
            _ => false,
        }
    }

    if !parse_value(b, &mut idx, 0) {
        return false;
    }
    skip_ws(b, &mut idx);
    idx == len
}

pub fn parse_runtime_ack(bytes: &str) -> Option<RuntimeAck> {
    if bytes.is_empty() || bytes.len() > 65536 {
        return None;
    }
    if !check_json_depth_and_no_dup_keys(bytes) {
        return None;
    }
    let v: Value = serde_json::from_str(bytes).ok()?;
    let obj = v.as_object()?;

    let int_in_range = |key: &str, min: i64, max: u64| -> Option<i64> {
        let val = obj.get(key)?;
        if let Some(u) = val.as_u64() {
            if u <= max && (min <= 0 || u >= min as u64) {
                return i64::try_from(u).ok();
            }
            return None;
        }
        let i = val.as_i64()?;
        if i >= min && (i < 0 || (i as u64) <= max) {
            Some(i)
        } else {
            None
        }
    };
    let u64_in_range = |key: &str, min: u64, max: u64| -> Option<u64> {
        let val = obj.get(key)?;
        let u = val.as_u64()?;
        if u >= min && u <= max {
            Some(u)
        } else {
            None
        }
    };

    let schema_version = int_in_range("schemaVersion", 1, 2)? as i32;
    let protocol_version = int_in_range("protocolVersion", 1, 2)? as i32;
    let engine = obj.get("engine")?.as_str()?.to_string();
    let process_name = obj.get("processName")?.as_str()?.to_string();
    let event_str = obj.get("eventType")?.as_str()?;
    let _ev = AckEventType::from_str_strict(event_str)?;
    let detail = obj.get("detail")?.as_str()?.to_string();

    let pid = int_in_range("pid", 1, libc::pid_t::MAX as u64)? as libc::pid_t;
    let uid = u64_in_range("uid", 0, libc::uid_t::MAX as u64)? as libc::uid_t;
    let config_generation = u64_in_range("configGeneration", 0, u64::MAX)?;
    let init_result = int_in_range("initResult", i32::MIN as i64, i32::MAX as u64)? as i32;
    let error_code = int_in_range("errorCode", i32::MIN as i64, i32::MAX as u64)? as i32;
    let timestamp = u64_in_range("timestamp", 0, u64::MAX)?;

    let mut out = RuntimeAck {
        schema_version,
        protocol_version,
        engine,
        process_name,
        pid,
        uid,
        event_type: event_str.to_string(),
        config_generation,
        init_result,
        error_code,
        timestamp,
        detail,
        ..Default::default()
    };

    if schema_version == 2 {
        out.boot_id = obj.get("bootId")?.as_str()?.to_string();
        out.config_hash = obj.get("configHash")?.as_str()?.to_string();
        out.request_id = obj.get("requestId")?.as_str()?.to_string();
        out.process_start_ticks = u64_in_range("processStartTicks", 1, u64::MAX)?;
    }

    Some(out)
}

pub fn validate_runtime_ack(ack: &RuntimeAck) -> Result<(), &'static str> {
    if ack.schema_version != 1 && ack.schema_version != 2 {
        return Err("Unsupported schemaVersion");
    }
    if ack.protocol_version != ack.schema_version {
        return Err("Unsupported protocolVersion");
    }
    if !matches!(ack.engine.as_str(), "hma" | "pif" | "copg" | "tee") {
        return Err("Invalid engine identifier");
    }
    if ack.process_name.is_empty()
        || ack.process_name.len() > 4096
        || ack.process_name.contains('\0')
    {
        return Err("Invalid processName");
    }
    if ack.pid <= 0 {
        return Err("Invalid PID");
    }

    if ack.schema_version == 2 {
        let is_lower_hex = |s: &str, len: usize| {
            s.len() == len
                && s.bytes()
                    .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
        };
        let valid_boot = ack.boot_id.len() == 36
            && ack.boot_id.bytes().enumerate().all(|(i, c)| {
                if matches!(i, 8 | 13 | 18 | 23) {
                    c == b'-'
                } else {
                    matches!(c, b'0'..=b'9' | b'a'..=b'f')
                }
            });
        if !valid_boot
            || !is_lower_hex(&ack.config_hash, 64)
            || !is_lower_hex(&ack.request_id, 32)
            || ack.process_start_ticks == 0
            || ack.timestamp == 0
            || ack.config_generation == 0
        {
            return Err("Incomplete runtime snapshot identity");
        }
    }

    if ack.event().is_none() {
        return Err("Invalid eventType");
    }

    match ack.engine.as_str() {
        "hma" => {
            if ack.process_name != "system_server" && ack.process_name != "android" {
                return Err("HMA engine ownership violation: process must be system_server");
            }
            if ack.uid != 1000 && ack.uid != 0 {
                return Err("HMA engine UID violation: UID must be 1000 or 0");
            }
        }
        "pif" => {
            if !matches!(
                ack.process_name.as_str(),
                "com.google.android.gms"
                    | "com.google.android.gms.unstable"
                    | "com.android.vending"
            ) {
                return Err("PIF engine ownership violation: process must be GMS");
            }
        }
        "tee" => {
            if !matches!(
                ack.process_name.as_str(),
                "com.google.android.gms" | "com.google.android.gms.unstable"
            ) {
                return Err("TEE engine ownership violation: process must be GMS");
            }
        }
        "copg" => {
            let base = ack
                .process_name
                .split(':')
                .next()
                .unwrap_or(&ack.process_name);
            if matches!(
                base,
                "com.google.android.gms"
                    | "com.google.android.gms.unstable"
                    | "com.android.vending"
                    | "system_server"
                    | "android"
            ) || base.starts_with("com.google.android.gms.")
                || base.starts_with("com.android.vending.")
            {
                return Err(
                    "COPG engine ownership violation: cannot target GMS, Vending, or system_server",
                );
            }
        }
        _ => return Err("Invalid engine identifier"),
    }

    Ok(())
}

pub fn runtime_ack_read_small_file(path: &str) -> Option<Vec<u8>> {
    let c_path = CString::new(path).ok()?;
    let fd = unsafe {
        libc::open(
            c_path.as_ptr(),
            libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC,
        )
    };
    if fd < 0 {
        return None;
    }
    let mut out = Vec::new();
    let mut buf = [0u8; 4096];
    let mut ok = true;
    loop {
        let n = unsafe { libc::read(fd, buf.as_mut_ptr().cast(), buf.len()) };
        if n == 0 {
            break;
        }
        if n < 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err == libc::EINTR {
                continue;
            }
            ok = false;
            break;
        }
        out.extend_from_slice(&buf[..n as usize]);
        if out.len() > 65536 {
            ok = false;
            break;
        }
    }
    unsafe { libc::close(fd) };
    if ok {
        Some(out)
    } else {
        None
    }
}

pub fn runtime_ack_boot_id() -> String {
    let Some(bytes) = runtime_ack_read_small_file("/proc/sys/kernel/random/boot_id") else {
        return String::new();
    };
    let mut s = String::from_utf8_lossy(&bytes).into_owned();
    if s.ends_with('\n') {
        s.pop();
    }
    s
}

pub fn runtime_ack_now_millis() -> u64 {
    let mut now: libc::timespec = unsafe { std::mem::zeroed() };
    let rc = unsafe { libc::clock_gettime(libc::CLOCK_BOOTTIME, &mut now) };
    if rc != 0 || now.tv_sec < 0 {
        return 0;
    }
    (now.tv_sec as u64)
        .saturating_mul(1000)
        .saturating_add((now.tv_nsec as u64) / 1_000_000)
}

pub fn runtime_ack_read_process_identity(
    pid: libc::pid_t,
) -> Option<(libc::uid_t, u64, String)> {
    if pid <= 0 {
        return None;
    }
    let base = format!("/proc/{pid}");
    let read_start = || -> Option<u64> {
        let stat_bytes = runtime_ack_read_small_file(&format!("{base}/stat"))?;
        let stat = String::from_utf8_lossy(&stat_bytes);
        let end = stat.rfind(')')?;
        let rest = stat.get(end + 2..)?;
        let mut iter = rest.split_whitespace();
        let mut token = "";
        for field in 3..=22 {
            token = iter.next()?;
            if field == 3 && matches!(token, "Z" | "X" | "x") {
                return None;
            }
        }
        let val: u64 = token.parse().ok()?;
        if val > 0 {
            Some(val)
        } else {
            None
        }
    };

    let before = read_start()?;
    let status_bytes = runtime_ack_read_small_file(&format!("{base}/status"))?;
    let status = String::from_utf8_lossy(&status_bytes);
    let pos = status.find("\nUid:")?;
    let mut creds = status[pos + 5..].split_whitespace();
    let _real: u64 = creds.next()?.parse().ok()?;
    let effective: u64 = creds.next()?.parse().ok()?;
    if effective > libc::uid_t::MAX as u64 {
        return None;
    }

    let cmdline_bytes = runtime_ack_read_small_file(&format!("{base}/cmdline"))?;
    if cmdline_bytes.is_empty() {
        return None;
    }
    let nul_pos = cmdline_bytes
        .iter()
        .position(|&b| b == 0)
        .unwrap_or(cmdline_bytes.len());
    let cmdline = String::from_utf8_lossy(&cmdline_bytes[..nul_pos]).into_owned();
    if cmdline.is_empty() {
        return None;
    }
    let after = read_start()?;
    if before != after {
        return None;
    }
    Some((effective as libc::uid_t, before, cmdline))
}

pub fn runtime_ack_has_live_identity(ack: &RuntimeAck) -> bool {
    if ack.schema_version != 2
        || validate_runtime_ack(ack).is_err()
        || ack.boot_id != runtime_ack_boot_id()
    {
        return false;
    }
    let Some((uid, start, name)) = runtime_ack_read_process_identity(ack.pid) else {
        return false;
    };
    if uid != ack.uid || start != ack.process_start_ticks || name != ack.process_name {
        return false;
    }
    let ticks = unsafe { libc::sysconf(libc::_SC_CLK_TCK) };
    if ticks <= 0 {
        return false;
    }
    let rate = ticks as u64;
    if start / rate > u64::MAX / 1000 {
        return false;
    }
    let start_millis = (start / rate) * 1000 + (start % rate) * 1000 / rate;
    let now = runtime_ack_now_millis();
    now > 0 && ack.timestamp >= start_millis && ack.timestamp <= now
}

pub fn bind_runtime_ack_to_current_process(
    ack: &mut RuntimeAck,
    generation: u64,
    config_bytes: &[u8],
    request_id: &str,
) -> bool {
    let mut bound = ack.clone();
    bound.schema_version = 2;
    bound.protocol_version = 2;
    bound.pid = unsafe { libc::getpid() };
    let Some((uid, ticks, name)) = runtime_ack_read_process_identity(bound.pid) else {
        return false;
    };
    bound.uid = uid;
    bound.process_start_ticks = ticks;
    bound.process_name = name;
    bound.boot_id = runtime_ack_boot_id();
    bound.timestamp = runtime_ack_now_millis();
    bound.config_generation = generation;
    bound.config_hash = config_sha256(config_bytes);
    bound.request_id = request_id.to_string();
    if !runtime_ack_has_live_identity(&bound) {
        return false;
    }
    *ack = bound;
    true
}

pub fn runtime_ack_matches_current(
    ack: &RuntimeAck,
    engine: &str,
    generation: u64,
    config_bytes: &[u8],
    request_id: &str,
) -> bool {
    generation > 0
        && !request_id.is_empty()
        && ack.engine == engine
        && ack.config_generation == generation
        && ack.request_id == request_id
        && ack.config_hash == config_sha256(config_bytes)
        && runtime_ack_has_live_identity(ack)
}

pub fn open_runtime_ack_directory(path: &str, create: bool) -> Option<libc::c_int> {
    if path.is_empty() {
        return None;
    }
    let root = CString::new(if path.starts_with('/') { "/" } else { "." }).ok()?;
    let mut fd = unsafe {
        libc::open(
            root.as_ptr(),
            libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC,
        )
    };
    if fd < 0 {
        return None;
    }

    let bytes = path.as_bytes();
    let mut start = 0usize;
    while start < bytes.len() {
        let end = bytes[start..]
            .iter()
            .position(|&b| b == b'/')
            .map(|p| start + p)
            .unwrap_or(bytes.len());
        let part = &path[start..end];
        let is_last = end == bytes.len();
        start = end + 1;
        if part.is_empty() || part == "." {
            continue;
        }
        if part == ".." {
            unsafe { libc::close(fd) };
            return None;
        }
        let Ok(c_part) = CString::new(part) else {
            unsafe { libc::close(fd) };
            return None;
        };
        if create && is_last {
            let rc = unsafe { libc::mkdirat(fd, c_part.as_ptr(), 0o755) };
            if rc != 0 {
                let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if err != libc::EEXIST {
                    unsafe { libc::close(fd) };
                    return None;
                }
            }
        }
        let next = unsafe {
            libc::openat(
                fd,
                c_part.as_ptr(),
                libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            )
        };
        unsafe { libc::close(fd) };
        if next < 0 {
            return None;
        }
        fd = next;
    }

    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    let euid = unsafe { libc::geteuid() };
    if unsafe { libc::fstat(fd, &mut st) } != 0
        || st.st_uid != euid
        || (st.st_mode & 0o022) != 0
    {
        unsafe { libc::close(fd) };
        return None;
    }
    Some(fd)
}

pub fn read_runtime_ack_at(dir_fd: libc::c_int, engine: &str) -> Option<RuntimeAck> {
    let flags = libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC;
    let target = CString::new(format!("{engine}.ack.json")).ok()?;
    let mut fd = unsafe { libc::openat(dir_fd, target.as_ptr(), flags) };
    if fd < 0 {
        let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
        if err == libc::ENOENT {
            let compat = CString::new(format!("{engine}.json")).ok()?;
            fd = unsafe { libc::openat(dir_fd, compat.as_ptr(), flags) };
        }
    }
    if fd < 0 {
        return None;
    }
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    let euid = unsafe { libc::geteuid() };
    if unsafe { libc::fstat(fd, &mut st) } != 0
        || (st.st_mode & libc::S_IFMT) != libc::S_IFREG
        || st.st_uid != euid
        || (st.st_mode & 0o022) != 0
        || st.st_size <= 0
        || st.st_size > 65536
    {
        unsafe { libc::close(fd) };
        return None;
    }
    let expected = st.st_size as usize;
    let mut bytes = vec![0u8; expected];
    let mut offset = 0usize;
    while offset < expected {
        let n = unsafe {
            libc::read(
                fd,
                bytes[offset..].as_mut_ptr().cast(),
                expected - offset,
            )
        };
        if n < 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err == libc::EINTR {
                continue;
            }
            break;
        }
        if n == 0 {
            break;
        }
        offset += n as usize;
    }
    let mut extra = [0u8; 1];
    let trailing = loop {
        let n = unsafe { libc::read(fd, extra.as_mut_ptr().cast(), 1) };
        if n < 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err == libc::EINTR {
                continue;
            }
        }
        break n;
    };
    unsafe { libc::close(fd) };
    if offset != expected || trailing != 0 {
        return None;
    }
    let text = std::str::from_utf8(&bytes).ok()?;
    let parsed = parse_runtime_ack(text)?;
    if validate_runtime_ack(&parsed).is_err() || parsed.engine != engine {
        return None;
    }
    Some(parsed)
}

pub fn runtime_ack_advances(previous: &RuntimeAck, next: &RuntimeAck) -> bool {
    if !runtime_ack_has_live_identity(next) {
        return false;
    }
    if previous.schema_version != 2 || previous.boot_id != next.boot_id {
        return true;
    }
    if next.config_generation < previous.config_generation {
        return false;
    }
    let same_process =
        next.pid == previous.pid && next.process_start_ticks == previous.process_start_ticks;
    let same_snapshot = next.config_generation == previous.config_generation
        && next.config_hash == previous.config_hash
        && next.request_id == previous.request_id;
    if same_process && same_snapshot {
        if next.timestamp < previous.timestamp
            || previous.event() == Some(AckEventType::ApplyFailed)
        {
            return false;
        }
        return match (next.event(), previous.event()) {
            (Some(n), Some(p)) => (n as i32) > (p as i32),
            _ => false,
        };
    }
    if next.timestamp < previous.timestamp {
        return false;
    }
    if same_process {
        return next.config_generation > previous.config_generation
            || (next.config_generation == previous.config_generation
                && next.request_id != previous.request_id);
    }
    !runtime_ack_has_live_identity(previous)
        || next.process_start_ticks > previous.process_start_ticks
        || (next.process_start_ticks == previous.process_start_ticks
            && next.timestamp > previous.timestamp)
}

pub fn publish_runtime_ack(
    runtime_dir: &str,
    ack: &RuntimeAck,
    current_request_id: &str,
) -> bool {
    if validate_runtime_ack(ack).is_err() || !runtime_ack_has_live_identity(ack) {
        return false;
    }
    let serialized = ack.to_json_pretty();
    if serialized.is_empty() || serialized.len() > 65536 {
        return false;
    }
    let Some(dir) = open_runtime_ack_directory(runtime_dir, true) else {
        return false;
    };
    let locked = loop {
        let rc = unsafe { libc::flock(dir, libc::LOCK_EX) };
        if rc == 0 {
            break true;
        }
        let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
        if err != libc::EINTR {
            break false;
        }
    };
    if !locked {
        unsafe { libc::close(dir) };
        return false;
    }

    if !runtime_ack_has_live_identity(ack) {
        unsafe { libc::close(dir) };
        return false;
    }
    if let Some(previous) = read_runtime_ack_at(dir, &ack.engine) {
        if (current_request_id.is_empty() || previous.request_id == current_request_id)
            && !runtime_ack_advances(&previous, ack)
        {
            unsafe { libc::close(dir) };
            return false;
        }
    }

    static SEQ: AtomicU64 = AtomicU64::new(0);
    let target = format!("{}.ack.json", ack.engine);
    let compat = format!("{}.json", ack.engine);
    let pid = unsafe { libc::getpid() };

    let mut staging = String::new();
    let mut fd = -1;
    for _ in 0..128 {
        staging = format!("{target}.tmp.{pid}.{}", SEQ.fetch_add(1, Ordering::Relaxed));
        let Ok(c_stg) = CString::new(staging.as_str()) else {
            unsafe { libc::close(dir) };
            return false;
        };
        fd = unsafe {
            libc::openat(
                dir,
                c_stg.as_ptr(),
                libc::O_CREAT | libc::O_EXCL | libc::O_WRONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                0o600,
            )
        };
        if fd >= 0 {
            break;
        }
        let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
        if err != libc::EEXIST {
            break;
        }
    }
    if fd < 0 {
        unsafe { libc::close(dir) };
        return false;
    }

    let bytes = serialized.as_bytes();
    let mut offset = 0usize;
    while offset < bytes.len() {
        let n = unsafe { libc::write(fd, bytes[offset..].as_ptr().cast(), bytes.len() - offset) };
        if n < 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err == libc::EINTR {
                continue;
            }
            break;
        }
        if n == 0 {
            break;
        }
        offset += n as usize;
    }
    let mut ok = offset == bytes.len() && unsafe { libc::fsync(fd) } == 0;
    if unsafe { libc::close(fd) } != 0 {
        ok = false;
    }

    let compat_staging = format!("{staging}.compat");
    if let (Ok(c_stg), Ok(c_cstg), Ok(c_tgt), Ok(c_cmp)) = (
        CString::new(staging.as_str()),
        CString::new(compat_staging.as_str()),
        CString::new(target.as_str()),
        CString::new(compat.as_str()),
    ) {
        if ok {
            ok = unsafe { libc::linkat(dir, c_stg.as_ptr(), dir, c_cstg.as_ptr(), 0) } == 0;
        }
        if ok {
            ok = unsafe { libc::renameat(dir, c_stg.as_ptr(), dir, c_tgt.as_ptr()) } == 0;
        }
        if ok {
            ok = unsafe { libc::renameat(dir, c_cstg.as_ptr(), dir, c_cmp.as_ptr()) } == 0;
        }
        if ok {
            ok = unsafe { libc::fsync(dir) } == 0;
        }
        unsafe {
            libc::unlinkat(dir, c_stg.as_ptr(), 0);
            libc::unlinkat(dir, c_cstg.as_ptr(), 0);
        }
    } else {
        ok = false;
    }
    unsafe { libc::close(dir) };
    ok
}

pub fn write_runtime_ack_direct(runtime_dir: &str, ack: &RuntimeAck) -> bool {
    publish_runtime_ack(runtime_dir, ack, "")
}

pub fn read_runtime_ack(runtime_dir: &str, engine: &str) -> Option<RuntimeAck> {
    if !matches!(engine, "hma" | "pif" | "tee" | "copg") {
        return None;
    }
    let dir = open_runtime_ack_directory(runtime_dir, false)?;
    let locked = loop {
        let rc = unsafe { libc::flock(dir, libc::LOCK_SH) };
        if rc == 0 {
            break true;
        }
        let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
        if err != libc::EINTR {
            break false;
        }
    };
    let res = if locked {
        read_runtime_ack_at(dir, engine)
    } else {
        None
    };
    unsafe { libc::close(dir) };
    res
}

pub fn os_str_to_string(p: &Path) -> String {
    String::from_utf8_lossy(p.as_os_str().as_bytes()).into_owned()
}
