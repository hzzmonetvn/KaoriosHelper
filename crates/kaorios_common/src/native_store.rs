// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::ffi::CString;
use std::sync::atomic::{AtomicU64, Ordering};

const JOURNAL_NAME: &str = ".native-transaction.json";
const JOURNAL_LIMIT: usize = 128 * 1024 * 1024;
const FILE_LIMIT: usize = 16 * 1024 * 1024;
const TX_TOTAL_LIMIT: usize = 32 * 1024 * 1024;

pub type Changes = BTreeMap<String, Option<String>>;

pub struct NativeStore {
    dir_fd: libc::c_int,
    lock_fd: libc::c_int,
}

impl Drop for NativeStore {
    fn drop(&mut self) {
        if self.lock_fd >= 0 {
            unsafe { libc::close(self.lock_fd) };
        }
        if self.dir_fd >= 0 {
            unsafe { libc::close(self.dir_fd) };
        }
    }
}

fn trusted(st: &libc::stat) -> bool {
    let euid = unsafe { libc::geteuid() };
    st.st_uid == euid && (st.st_mode & 0o022) == 0
}

impl NativeStore {
    pub fn open(path: &str) -> Result<Self, String> {
        let c_path = CString::new(path).map_err(|_| "Untrusted data directory".to_string())?;
        let dir_fd = unsafe {
            libc::open(
                c_path.as_ptr(),
                libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            )
        };
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if dir_fd < 0 || unsafe { libc::fstat(dir_fd, &mut st) } != 0 || !trusted(&st) {
            if dir_fd >= 0 {
                unsafe { libc::close(dir_fd) };
            }
            return Err("Untrusted data directory".to_string());
        }

        let lock_name = CString::new(".config.lock").unwrap();
        let lock_fd = unsafe {
            libc::openat(
                dir_fd,
                lock_name.as_ptr(),
                libc::O_CREAT | libc::O_RDWR | libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC,
                0o600,
            )
        };
        let lock_ok = if lock_fd >= 0
            && unsafe { libc::fstat(lock_fd, &mut st) } == 0
            && crate::is_regular_file(u32::from(st.st_mode))
            && trusted(&st)
            && st.st_nlink == 1
        {
            loop {
                let rc = unsafe { libc::flock(lock_fd, libc::LOCK_EX) };
                if rc == 0 {
                    break true;
                }
                let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if err != libc::EINTR {
                    break false;
                }
            }
        } else {
            false
        };

        if !lock_ok {
            if lock_fd >= 0 {
                unsafe { libc::close(lock_fd) };
            }
            unsafe { libc::close(dir_fd) };
            return Err("Configuration lock failed".to_string());
        }

        let mut store = Self { dir_fd, lock_fd };
        store.recover()?;
        Ok(store)
    }

    pub fn check(&self, name: &str) -> Result<(), String> {
        if name.is_empty()
            || name == "."
            || name == ".."
            || name.contains('/')
            || name.contains('\0')
        {
            return Err("Invalid configuration leaf name".to_string());
        }
        let c_name = CString::new(name).map_err(|_| "Invalid configuration leaf name".to_string())?;
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstatat(self.dir_fd, c_name.as_ptr(), &mut st, libc::AT_SYMLINK_NOFOLLOW) } != 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err == libc::ENOENT {
                return Ok(());
            }
            return Err("Target inspection failed".to_string());
        }
        if !crate::is_regular_file(u32::from(st.st_mode)) || !trusted(&st) || st.st_nlink != 1 {
            return Err("Untrusted configuration file".to_string());
        }
        Ok(())
    }

    pub fn exists(&self, name: &str) -> Result<bool, String> {
        self.check(name)?;
        let c_name = CString::new(name).map_err(|_| "Invalid configuration leaf name".to_string())?;
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstatat(self.dir_fd, c_name.as_ptr(), &mut st, libc::AT_SYMLINK_NOFOLLOW) } == 0 {
            return Ok(true);
        }
        let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
        if err != libc::ENOENT {
            return Err("Target inspection failed".to_string());
        }
        Ok(false)
    }

    pub fn read(&self, name: &str, optional: bool) -> Result<String, String> {
        self.read_with_limit(name, optional, FILE_LIMIT)
    }

    pub fn read_with_limit(
        &self,
        name: &str,
        optional: bool,
        limit: usize,
    ) -> Result<String, String> {
        self.check(name)?;
        let c_name = CString::new(name).map_err(|_| "Invalid configuration leaf name".to_string())?;
        let fd = unsafe {
            libc::openat(
                self.dir_fd,
                c_name.as_ptr(),
                libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC,
            )
        };
        if fd < 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if optional && err == libc::ENOENT {
                return Ok(String::new());
            }
            return Err("Configuration read failed".to_string());
        }
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(fd, &mut st) } != 0
            || !crate::is_regular_file(u32::from(st.st_mode))
            || !trusted(&st)
            || st.st_nlink != 1
            || st.st_size < 0
            || (st.st_size as u64) > limit as u64
        {
            unsafe { libc::close(fd) };
            return Err("Untrusted configuration file".to_string());
        }

        let mut out = Vec::new();
        let mut buf = [0u8; 8192];
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
                unsafe { libc::close(fd) };
                return Err("Configuration read failed".to_string());
            }
            out.extend_from_slice(&buf[..n as usize]);
            if out.len() > limit {
                unsafe { libc::close(fd) };
                return Err("Configuration too large".to_string());
            }
        }
        unsafe { libc::close(fd) };
        String::from_utf8(out).map_err(|_| "Configuration read failed".to_string())
    }

    pub fn write(&self, name: &str, value: &str) -> Result<(), String> {
        self.check(name)?;
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let pid = unsafe { libc::getpid() };
        let mut tmp = String::new();
        let mut fd = -1;
        for _ in 0..100 {
            let c = COUNTER.fetch_add(1, Ordering::Relaxed) + 1;
            tmp = format!(".staging.{pid}.{c}");
            let c_tmp = CString::new(tmp.as_str()).map_err(|_| "Staging creation failed".to_string())?;
            fd = unsafe {
                libc::openat(
                    self.dir_fd,
                    c_tmp.as_ptr(),
                    libc::O_WRONLY | libc::O_CREAT | libc::O_EXCL | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                    0o600,
                )
            };
            if fd >= 0 {
                break;
            }
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err != libc::EEXIST {
                return Err("Staging creation failed".to_string());
            }
        }
        if fd < 0 {
            return Err("Staging creation failed".to_string());
        }

        let mut ok = unsafe { libc::fchmod(fd, 0o600) } == 0;
        let bytes = value.as_bytes();
        let mut pos = 0usize;
        while ok && pos < bytes.len() {
            let n = unsafe { libc::write(fd, bytes[pos..].as_ptr().cast(), bytes.len() - pos) };
            if n < 0 {
                let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if err == libc::EINTR {
                    continue;
                }
                ok = false;
                break;
            }
            if n == 0 {
                ok = false;
                break;
            }
            pos += n as usize;
        }
        if ok {
            ok = unsafe { libc::fsync(fd) } == 0;
        }
        if unsafe { libc::close(fd) } != 0 {
            ok = false;
        }

        let c_tmp = CString::new(tmp.as_str()).map_err(|_| "Configuration publication failed".to_string())?;
        let c_name = CString::new(name).map_err(|_| "Configuration publication failed".to_string())?;
        if ok {
            ok = unsafe { libc::renameat(self.dir_fd, c_tmp.as_ptr(), self.dir_fd, c_name.as_ptr()) } == 0;
        }
        if !ok {
            unsafe { libc::unlinkat(self.dir_fd, c_tmp.as_ptr(), 0) };
            return Err("Configuration publication failed".to_string());
        }
        if unsafe { libc::fsync(self.dir_fd) } != 0 {
            return Err("Directory sync failed".to_string());
        }
        Ok(())
    }

    pub fn remove(&self, name: &str) -> Result<(), String> {
        self.check(name)?;
        let c_name = CString::new(name).map_err(|_| "Configuration removal failed".to_string())?;
        if unsafe { libc::unlinkat(self.dir_fd, c_name.as_ptr(), 0) } != 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err != libc::ENOENT {
                return Err("Configuration removal failed".to_string());
            }
        }
        if unsafe { libc::fsync(self.dir_fd) } != 0 {
            return Err("Directory sync failed".to_string());
        }
        Ok(())
    }

    pub fn transaction(&self, changes: &Changes) -> Result<(), String> {
        if changes.is_empty() {
            return Ok(());
        }
        self.validate_changes(changes)?;
        let mut entries = serde_json::Map::new();
        for (name, val) in changes {
            match val {
                Some(s) => {
                    entries.insert(name.clone(), Value::String(s.clone()));
                }
                None => {
                    entries.insert(name.clone(), Value::Null);
                }
            }
        }
        let journal = json!({
            "schemaVersion": 1,
            "files": Value::Object(entries),
        })
        .to_string();
        if journal.len() > JOURNAL_LIMIT {
            return Err("Configuration transaction too large".to_string());
        }
        self.write(JOURNAL_NAME, &journal)?;
        self.apply(changes)
    }

    fn transaction_leaf(name: &str) -> bool {
        for filename in ["hma.json", "pif.json", "keybox.xml", "spoof.json", "kaorios.json"] {
            if name == filename || name == format!("{filename}.lkg") {
                return true;
            }
        }
        for engine in ["hma", "pif", "tee", "copg", "kaorios"] {
            for suffix in [
                ".stored_gen",
                ".requested_gen",
                ".request.json",
                ".reload_pending",
                ".verified",
                ".enabled",
                ".disabled",
            ] {
                if name == format!("{engine}{suffix}") {
                    return true;
                }
            }
        }
        false
    }

    fn validate_changes(&self, changes: &Changes) -> Result<(), String> {
        if changes.is_empty() || changes.len() > 64 {
            return Err("Invalid configuration transaction".to_string());
        }
        let mut total = 0usize;
        for (name, value) in changes {
            if !Self::transaction_leaf(name) {
                return Err("Invalid transaction target".to_string());
            }
            self.check(name)?;
            if let Some(v) = value {
                if v.len() > FILE_LIMIT {
                    return Err("Configuration too large".to_string());
                }
                total += v.len();
            }
        }
        if total > TX_TOTAL_LIMIT {
            return Err("Configuration transaction too large".to_string());
        }
        Ok(())
    }

    fn apply(&self, changes: &Changes) -> Result<(), String> {
        for (applied, (name, value)) in changes.iter().enumerate() {
            #[cfg(feature = "test-faults")]
            if let Ok(fault) = std::env::var("KAORIOS_TEST_TX_STOP_AFTER") {
                if applied.to_string() == fault {
                    unsafe { libc::_exit(86) };
                }
            }
            #[cfg(not(feature = "test-faults"))]
            let _ = applied;
            if let Some(v) = value {
                self.write(name, v)?;
            } else {
                self.remove(name)?;
            }
        }
        self.remove(JOURNAL_NAME)
    }

    fn recover(&mut self) -> Result<(), String> {
        let text = self.read_with_limit(JOURNAL_NAME, true, JOURNAL_LIMIT)?;
        if text.is_empty() {
            if self.exists(JOURNAL_NAME)? {
                return Err("Invalid configuration journal".to_string());
            }
            return Ok(());
        }
        let journal: Value = serde_json::from_str(&text)
            .map_err(|_| "Invalid configuration journal".to_string())?;
        let obj = journal
            .as_object()
            .ok_or_else(|| "Invalid configuration journal".to_string())?;
        if obj.len() != 2
            || obj.get("schemaVersion").and_then(|v| v.as_i64()) != Some(1)
        {
            return Err("Invalid configuration journal".to_string());
        }
        let files = obj
            .get("files")
            .and_then(|v| v.as_object())
            .ok_or_else(|| "Invalid configuration journal".to_string())?;
        let mut changes = Changes::new();
        for (k, v) in files {
            if v.is_null() {
                changes.insert(k.clone(), None);
            } else if let Some(s) = v.as_str() {
                changes.insert(k.clone(), Some(s.to_string()));
            } else {
                return Err("Invalid configuration journal".to_string());
            }
        }
        self.validate_changes(&changes)?;
        self.apply(&changes)
    }
}

pub fn validate_native_json(engine: &str, text: &str) -> bool {
    let Ok(j) = serde_json::from_str::<Value>(text) else {
        return false;
    };
    let Some(obj) = j.as_object() else {
        return false;
    };
    if matches!(engine, "pif" | "copg") {
        if let Some(v) = obj.get("enabled") {
            if !v.is_boolean() {
                return false;
            }
        }
    }
    if engine == "kaorios" {
        if let Some(v) = obj.get("schemaVersion") {
            if v.as_i64() != Some(1) {
                return false;
            }
        }
    }
    if engine == "hma" {
        if let Some(v) = obj.get("configVersion") {
            if !v.is_i64() && !v.is_u64() {
                return false;
            }
        }
        for k in ["templates", "settingsTemplates", "scope"] {
            if let Some(v) = obj.get(k) {
                if !v.is_object() {
                    return false;
                }
            }
        }
    }
    if engine == "pif" {
        for k in [
            "MANUFACTURER",
            "MODEL",
            "FINGERPRINT",
            "BRAND",
            "PRODUCT",
            "DEVICE",
            "BOARD",
            "HARDWARE",
            "VERSION.RELEASE",
            "VERSION.INCREMENTAL",
            "VERSION.SECURITY_PATCH",
        ] {
            if let Some(v) = obj.get(k) {
                if !v.is_string() {
                    return false;
                }
            }
        }
        let valid_pif_flag = |v: &Value| -> bool {
            if v.is_boolean() || v.is_i64() || v.is_u64() {
                return true;
            }
            let Some(raw) = v.as_str() else {
                return false;
            };
            let trimmed = raw.trim().to_ascii_lowercase();
            matches!(trimmed.as_str(), "0" | "1" | "true" | "false")
        };
        for k in ["spoofBuild", "spoofProps", "spoofProvider", "spoofSignature"] {
            if let Some(v) = obj.get(k) {
                if !valid_pif_flag(v) {
                    return false;
                }
            }
        }
    }
    if engine == "copg" {
        if let Some(cpu) = obj.get("cpu_spoof") {
            let Some(cpu_obj) = cpu.as_object() else {
                return false;
            };
            for k in ["blacklist", "cpu_only_packages"] {
                if let Some(v) = cpu_obj.get(k) {
                    if !v.is_array() {
                        return false;
                    }
                }
            }
        }
        for section in ["profiles", "packages"] {
            let Some(sec_val) = obj.get(section) else {
                continue;
            };
            if section == "packages" {
                if let Some(arr) = sec_val.as_array() {
                    if !arr.iter().all(|pkg| pkg.is_string()) {
                        return false;
                    }
                    continue;
                }
            }
            let Some(profiles) = sec_val.as_object() else {
                return false;
            };
            for profile in profiles.values() {
                let Some(pobj) = profile.as_object() else {
                    return false;
                };
                for flag in [
                    "enabled",
                    "device_spoof",
                    "cpu_spoof",
                    "plt_hook",
                    "android_id_spoof",
                ] {
                    if let Some(v) = pobj.get(flag) {
                        if !v.is_boolean() {
                            return false;
                        }
                    }
                }
                for field in [
                    "model",
                    "brand",
                    "manufacturer",
                    "device",
                    "product",
                    "fingerprint",
                    "release",
                    "serial",
                    "android_id",
                ] {
                    if let Some(v) = pobj.get(field) {
                        if !v.is_string() {
                            return false;
                        }
                    }
                }
                if let Some(sdk) = pobj.get("sdk_int") {
                    let Some(n) = sdk.as_i64() else {
                        return false;
                    };
                    if n < 0 || n > i32::MAX as i64 {
                        return false;
                    }
                }
                if let Some(props) = pobj.get("props") {
                    let Some(props_obj) = props.as_object() else {
                        return false;
                    };
                    if !props_obj.values().all(|v| v.is_string()) {
                        return false;
                    }
                }
            }
        }
        for (k, v) in obj {
            if !k.starts_with("PACKAGES_") {
                continue;
            }
            let is_device = k.ends_with("_DEVICE");
            if is_device {
                if !v.is_object() {
                    return false;
                }
            } else if !v.is_array() {
                return false;
            }
        }
    }
    true
}
