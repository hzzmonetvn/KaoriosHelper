// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use crate::engine::Engine;
use crate::jni_sys::{
    delete_local_ref, exception_check_and_clear, find_class, get_field_id, get_method_id,
    get_static_field_id, jint, jni_vtbl, jobject, new_string_utf, set_static_int_field,
    set_static_string_field, JNIEnv,
};
use crate::shadowhook::{
    shadowhook_get_errno, shadowhook_hook_sym_addr, shadowhook_init, SHADOWHOOK_ERRNO_PENDING,
    SHADOWHOOK_MODE_SHARED,
};
use crate::zygisk_abi::{AppSpecializeArgs, ZygiskApi, ZygiskOption};
use kaorios_common::ack_protocol::{AckEventType, RuntimeAck};
use kaorios_common::companion_ipc::{CompanionConnector, CompanionSession};
use kaorios_common::runtime_snapshot::{
    bind_runtime_ack_to_snapshot, read_runtime_config_snapshot,
    write_runtime_ack_for_current_config, RuntimeConfigSnapshot,
};
use kaorios_common::{log_debug, log_error, log_info, log_warn, DEFAULT_DATA_DIR};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
#[cfg_attr(not(target_os = "android"), allow(unused_imports))]
use std::ffi::{CStr, CString};
use std::fs;
#[cfg_attr(not(target_os = "android"), allow(unused_imports))]
use std::os::raw::{c_char, c_int, c_void};
use std::path::Path;
use std::sync::atomic::{AtomicPtr, AtomicU64, Ordering};
use std::sync::Mutex;

static COPG_ACTIVE_INSTANCE: AtomicPtr<CopgEngine> = AtomicPtr::new(std::ptr::null_mut());
#[cfg_attr(
    not(all(
        target_os = "android",
        any(target_arch = "arm", target_arch = "aarch64")
    )),
    allow(dead_code)
)]
static ORIG_PLT_PROP_GET: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
#[cfg_attr(
    not(all(
        target_os = "android",
        any(target_arch = "arm", target_arch = "aarch64")
    )),
    allow(dead_code)
)]
static ORIG_LIBC_PROP_GET: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
#[cfg_attr(
    not(all(
        target_os = "android",
        any(target_arch = "arm", target_arch = "aarch64")
    )),
    allow(dead_code)
)]
static ORIG_LIBC_READ_CB: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());

#[cfg(all(
    target_os = "android",
    any(target_arch = "arm", target_arch = "aarch64")
))]
const PROP_VALUE_MAX: usize = 92;

#[cfg(all(
    target_os = "android",
    any(target_arch = "arm", target_arch = "aarch64")
))]
type CopgPropCallback = unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, u32);

#[cfg(all(
    target_os = "android",
    any(target_arch = "arm", target_arch = "aarch64")
))]
#[repr(C)]
struct CopgCallbackContext {
    original_callback: CopgPropCallback,
    original_cookie: *mut c_void,
}

#[cfg(all(
    target_os = "android",
    any(target_arch = "arm", target_arch = "aarch64")
))]
unsafe extern "C" fn my_copg_system_property_get(name: *const c_char, value: *mut c_char) -> c_int {
    if !name.is_null() && !value.is_null() {
        let eng_ptr = COPG_ACTIVE_INSTANCE.load(Ordering::Acquire);
        if !eng_ptr.is_null() {
            let eng = &*eng_ptr;
            let name_str = CStr::from_ptr(name).to_string_lossy();
            if let Some(override_val) = eng.resolve_property_override(&name_str) {
                eng.record_property_intercepted();
                let bytes = override_val.as_bytes();
                let len = bytes.len().min(PROP_VALUE_MAX - 1);
                std::ptr::copy_nonoverlapping(bytes.as_ptr(), value.cast(), len);
                *value.add(len) = 0;
                log_debug!("COPG [PLT Hook] [{name_str}]: -> {override_val}");
                return len as c_int;
            }
        }
    }
    let orig_ptr = ORIG_PLT_PROP_GET.load(Ordering::Acquire);
    if !orig_ptr.is_null() {
        let orig: unsafe extern "C" fn(*const c_char, *mut c_char) -> c_int =
            std::mem::transmute(orig_ptr);
        return orig(name, value);
    }
    -1
}

#[cfg(all(
    target_os = "android",
    any(target_arch = "arm", target_arch = "aarch64")
))]
unsafe extern "C" fn copg_shadow_property_get(name: *const c_char, value: *mut c_char) -> c_int {
    if !name.is_null() && !value.is_null() {
        let eng_ptr = COPG_ACTIVE_INSTANCE.load(Ordering::Acquire);
        if !eng_ptr.is_null() {
            let eng = &*eng_ptr;
            let name_str = CStr::from_ptr(name).to_string_lossy();
            if let Some(override_val) = eng.resolve_property_override(&name_str) {
                eng.record_property_intercepted();
                let bytes = override_val.as_bytes();
                let len = bytes.len().min(PROP_VALUE_MAX - 1);
                std::ptr::copy_nonoverlapping(bytes.as_ptr(), value.cast(), len);
                *value.add(len) = 0;
                log_debug!("COPG [ShadowHook libc get] [{name_str}]: -> {override_val}");
                return len as c_int;
            }
        }
    }
    let orig_ptr = ORIG_LIBC_PROP_GET.load(Ordering::Acquire);
    if !orig_ptr.is_null() {
        let orig: unsafe extern "C" fn(*const c_char, *mut c_char) -> c_int =
            std::mem::transmute(orig_ptr);
        return orig(name, value);
    }
    0
}

#[cfg(all(
    target_os = "android",
    any(target_arch = "arm", target_arch = "aarch64")
))]
unsafe extern "C" fn copg_modify_read_callback(
    cookie: *mut c_void,
    name: *const c_char,
    value: *const c_char,
    serial: u32,
) {
    if cookie.is_null() || name.is_null() || value.is_null() {
        return;
    }
    let ctx = &*(cookie as *const CopgCallbackContext);
    let eng_ptr = COPG_ACTIVE_INSTANCE.load(Ordering::Acquire);
    if !eng_ptr.is_null() {
        let eng = &*eng_ptr;
        let name_str = CStr::from_ptr(name).to_string_lossy();
        if let Some(override_val) = eng.resolve_property_override(&name_str) {
            eng.record_property_intercepted();
            if let Ok(c_ov) = CString::new(override_val.as_str()) {
                log_debug!("COPG [ShadowHook libc cb] [{name_str}] -> {override_val}");
                (ctx.original_callback)(ctx.original_cookie, name, c_ov.as_ptr(), serial);
                return;
            }
        }
    }
    (ctx.original_callback)(ctx.original_cookie, name, value, serial);
}

#[cfg(all(
    target_os = "android",
    any(target_arch = "arm", target_arch = "aarch64")
))]
unsafe extern "C" fn copg_shadow_read_callback(
    pi: *const c_void,
    callback: Option<CopgPropCallback>,
    cookie: *mut c_void,
) {
    let orig_ptr = ORIG_LIBC_READ_CB.load(Ordering::Acquire);
    let Some(cb) = callback else {
        return;
    };
    if orig_ptr.is_null() || pi.is_null() {
        return;
    }
    let orig: unsafe extern "C" fn(*const c_void, CopgPropCallback, *mut c_void) =
        std::mem::transmute(orig_ptr);
    let mut ctx = CopgCallbackContext {
        original_callback: cb,
        original_cookie: cookie,
    };
    orig(
        pi,
        copg_modify_read_callback,
        (&mut ctx as *mut CopgCallbackContext).cast(),
    );
}

#[derive(Debug, Clone, Default)]
pub struct CopgDeviceInfo {
    pub model: String,
    pub brand: String,
    pub device: String,
    pub manufacturer: String,
    pub fingerprint: String,
    pub product: String,
    pub release: String,
    pub serial: String,
    pub android_id: String,
    pub sdk_int: i32,
    pub should_spoof_serial: bool,
    pub should_spoof_android_id: bool,
    pub should_spoof_version: bool,
    pub should_spoof_sdk_int: bool,
    pub prop_overrides: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct CopgPackageFlags {
    pub needs_device_spoof: bool,
    pub needs_cpu_mount: bool,
    pub needs_cpu_unmount: bool,
    pub needs_plt_hook: bool,
    pub needs_android_id: bool,
}

fn populate_device_info_from_json(
    prof: &serde_json::Map<String, Value>,
    info: &mut CopgDeviceInfo,
    flags: &mut CopgPackageFlags,
) {
    let read_str = |lower_k: &str, upper_k: &str, dst: &mut String| {
        if let Some(s) = prof.get(lower_k).and_then(|v| v.as_str()) {
            *dst = s.to_string();
        } else if let Some(s) = prof.get(upper_k).and_then(|v| v.as_str()) {
            *dst = s.to_string();
        }
    };

    read_str("model", "MODEL", &mut info.model);
    read_str("brand", "BRAND", &mut info.brand);
    read_str("manufacturer", "MANUFACTURER", &mut info.manufacturer);
    read_str("device", "DEVICE", &mut info.device);
    read_str("product", "PRODUCT", &mut info.product);
    read_str("fingerprint", "FINGERPRINT", &mut info.fingerprint);
    read_str("release", "RELEASE", &mut info.release);
    info.should_spoof_version = !info.release.is_empty();

    let mut serial_val = String::new();
    read_str("serial", "SERIAL", &mut serial_val);
    if !serial_val.is_empty() {
        info.serial = serial_val;
        info.should_spoof_serial = true;
    }

    if let Some(sdk) = prof.get("sdk_int").and_then(|v| v.as_i64()) {
        info.sdk_int = sdk as i32;
        info.should_spoof_sdk_int = info.sdk_int > 0;
    } else if let Some(sdk) = prof.get("SDK_INT").and_then(|v| v.as_i64()) {
        info.sdk_int = sdk as i32;
        info.should_spoof_sdk_int = info.sdk_int > 0;
    }

    let mut aid_val = String::new();
    read_str("android_id", "ANDROID_ID", &mut aid_val);
    if !aid_val.is_empty() {
        info.android_id = aid_val;
        let aid_flag = prof
            .get("android_id_spoof")
            .and_then(|v| v.as_bool())
            .unwrap_or(true);
        info.should_spoof_android_id = aid_flag && !info.android_id.is_empty();
        flags.needs_android_id = info.should_spoof_android_id;
    }

    for props_key in ["props", "PROPS"] {
        if let Some(pobj) = prof.get(props_key).and_then(|v| v.as_object()) {
            for (pk, pv) in pobj {
                if let Some(s) = pv.as_str() {
                    info.prop_overrides.insert(pk.clone(), s.to_string());
                }
            }
        }
    }
}

#[derive(Default)]
struct CopgConfigState {
    config_snapshot: RuntimeConfigSnapshot,
    applied_snapshot: RuntimeConfigSnapshot,
    target_packages: BTreeSet<String>,
    cpu_blacklist: BTreeSet<String>,
    cpu_only_packages: BTreeSet<String>,
    package_configs: BTreeMap<String, (CopgDeviceInfo, CopgPackageFlags)>,
}

pub struct CopgEngine {
    data_dir: String,
    api: Option<ZygiskApi>,
    env: *mut JNIEnv,
    companion_session: CompanionSession,
    state: Mutex<CopgConfigState>,
    current_device_info: CopgDeviceInfo,
    current_flags: CopgPackageFlags,
    active_for_current_process: bool,
    properties_intercepted: AtomicU64,
    plt_commit_succeeded: bool,
    late_hook_installed: bool,
    cpu_mount_active: bool,
    libraries_discovered: usize,
    hooks_attempted: usize,
    hooks_registered: usize,
}

unsafe impl Send for CopgEngine {}
unsafe impl Sync for CopgEngine {}

impl Default for CopgEngine {
    fn default() -> Self {
        Self::new(DEFAULT_DATA_DIR.to_string())
    }
}

impl Drop for CopgEngine {
    fn drop(&mut self) {
        let _ = COPG_ACTIVE_INSTANCE.compare_exchange(
            self as *mut Self,
            std::ptr::null_mut(),
            Ordering::AcqRel,
            Ordering::Relaxed,
        );
        self.companion_session.close_session();
    }
}

impl CopgEngine {
    pub fn new(data_dir: String) -> Self {
        Self {
            data_dir,
            api: None,
            env: std::ptr::null_mut(),
            companion_session: CompanionSession::new(),
            state: Mutex::new(CopgConfigState::default()),
            current_device_info: CopgDeviceInfo::default(),
            current_flags: CopgPackageFlags::default(),
            active_for_current_process: false,
            properties_intercepted: AtomicU64::new(0),
            plt_commit_succeeded: false,
            late_hook_installed: false,
            cpu_mount_active: false,
            libraries_discovered: 0,
            hooks_attempted: 0,
            hooks_registered: 0,
        }
    }

    pub fn extract_base_package(process_name: &str) -> &str {
        process_name.split(':').next().unwrap_or(process_name)
    }

    pub fn is_package_allowed(&self, package_name: &str) -> bool {
        let base = Self::extract_base_package(package_name);
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
            log_warn!(
                "COPG: Disallowed target package '{package_name}' blocked by Hook Ownership Isolation"
            );
            return false;
        }
        true
    }

    pub fn record_property_intercepted(&self) {
        self.properties_intercepted.fetch_add(1, Ordering::Relaxed);
    }

    pub fn get_intercepted_count(&self) -> u64 {
        self.properties_intercepted.load(Ordering::Relaxed)
    }

    pub fn resolve_property_override(&self, name: &str) -> Option<String> {
        if !self.active_for_current_process {
            return None;
        }
        if let Some(v) = self.current_device_info.prop_overrides.get(name) {
            return Some(v.clone());
        }
        if let Some(mut field) = name.strip_prefix("ro.product.") {
            for part in ["system.", "vendor.", "odm.", "product.", "system_ext."] {
                if let Some(stripped) = field.strip_prefix(part) {
                    field = stripped;
                    break;
                }
            }
            match field {
                "model" if !self.current_device_info.model.is_empty() => {
                    return Some(self.current_device_info.model.clone());
                }
                "brand" if !self.current_device_info.brand.is_empty() => {
                    return Some(self.current_device_info.brand.clone());
                }
                "manufacturer" if !self.current_device_info.manufacturer.is_empty() => {
                    return Some(self.current_device_info.manufacturer.clone());
                }
                "device" if !self.current_device_info.device.is_empty() => {
                    return Some(self.current_device_info.device.clone());
                }
                "name" if !self.current_device_info.product.is_empty() => {
                    return Some(self.current_device_info.product.clone());
                }
                _ => {}
            }
        }
        if name == "ro.build.fingerprint" && !self.current_device_info.fingerprint.is_empty() {
            return Some(self.current_device_info.fingerprint.clone());
        }
        if name == "ro.build.product" {
            let prod = if !self.current_device_info.product.is_empty() {
                &self.current_device_info.product
            } else {
                &self.current_device_info.device
            };
            if !prod.is_empty() {
                return Some(prod.clone());
            }
        }
        if name == "ro.build.version.release" && !self.current_device_info.release.is_empty() {
            return Some(self.current_device_info.release.clone());
        }
        if name == "ro.build.version.sdk"
            && self.current_device_info.should_spoof_sdk_int
            && self.current_device_info.sdk_int > 0
        {
            return Some(self.current_device_info.sdk_int.to_string());
        }
        if (name == "ro.serialno" || name == "ro.boot.serialno")
            && self.current_device_info.should_spoof_serial
            && !self.current_device_info.serial.is_empty()
        {
            return Some(self.current_device_info.serial.clone());
        }
        None
    }

    pub fn get_package_config(&self, pkg: &str) -> Option<(CopgDeviceInfo, CopgPackageFlags)> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(found) = st.package_configs.get(pkg) {
            return Some(found.clone());
        }
        if pkg.contains(':') {
            return st
                .package_configs
                .get(Self::extract_base_package(pkg))
                .cloned();
        }
        None
    }

    pub fn reload_config_if_needed(&self, force: bool) -> bool {
        let Ok(snapshot) = read_runtime_config_snapshot(&self.data_dir, "copg", "spoof.json")
        else {
            log_warn!(
                "COPG: Configuration snapshot unavailable; retaining existing known-good config"
            );
            return false;
        };
        self.apply_snapshot(snapshot, force)
    }

    pub fn apply_snapshot(&self, snapshot: RuntimeConfigSnapshot, force: bool) -> bool {
        if snapshot.bytes.is_empty() {
            return false;
        }
        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        if !force && st.config_snapshot.same_identity(&snapshot) {
            return true;
        }
        let Ok(j) = serde_json::from_str::<Value>(&snapshot.bytes) else {
            log_warn!(
                "COPG: Malformed JSON in {}/spoof.json; retaining existing known-good config",
                self.data_dir
            );
            return false;
        };
        let Some(root) = j.as_object() else {
            log_warn!(
                "COPG: Malformed JSON in {}/spoof.json; retaining existing known-good config",
                self.data_dir
            );
            return false;
        };

        if root.get("enabled").and_then(|v| v.as_bool()) == Some(false) {
            log_info!(
                "COPG: Feature globally disabled in {}/spoof.json",
                self.data_dir
            );
            st.target_packages.clear();
            st.package_configs.clear();
            st.cpu_blacklist.clear();
            st.cpu_only_packages.clear();
            st.config_snapshot = snapshot;
            return true;
        }

        let mut new_targets = BTreeSet::new();
        let mut new_configs: BTreeMap<String, (CopgDeviceInfo, CopgPackageFlags)> = BTreeMap::new();
        let mut new_blacklist = BTreeSet::new();
        let mut new_cpu_only = BTreeSet::new();

        let profiles_obj = root
            .get("profiles")
            .and_then(|v| v.as_object())
            .or_else(|| root.get("packages").and_then(|v| v.as_object()));

        if let Some(profiles) = profiles_obj {
            for (pkg_name, prof_val) in profiles {
                if !pkg_name.contains('.') || pkg_name.contains('/') {
                    log_warn!("COPG: Skipping invalid package identifier: {pkg_name}");
                    continue;
                }
                if !self.is_package_allowed(pkg_name) {
                    continue;
                }
                let Some(prof) = prof_val.as_object() else {
                    continue;
                };
                if prof.get("enabled").and_then(|v| v.as_bool()) == Some(false) {
                    continue;
                }
                let mut info = CopgDeviceInfo::default();
                let mut flags = CopgPackageFlags {
                    needs_device_spoof: prof
                        .get("device_spoof")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(true),
                    needs_cpu_mount: prof
                        .get("cpu_spoof")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false),
                    needs_plt_hook: false,
                    needs_cpu_unmount: false,
                    needs_android_id: false,
                };
                flags.needs_plt_hook = prof
                    .get("plt_hook")
                    .and_then(|v| v.as_bool())
                    .unwrap_or(flags.needs_device_spoof);
                populate_device_info_from_json(prof, &mut info, &mut flags);
                new_targets.insert(pkg_name.clone());
                new_configs.insert(pkg_name.clone(), (info, flags));
            }
        }

        for (key, val) in root {
            let Some(arr) = val.as_array() else {
                continue;
            };
            let mut group_info = CopgDeviceInfo::default();
            let mut group_flags = CopgPackageFlags {
                needs_device_spoof: true,
                needs_cpu_mount: false,
                needs_plt_hook: true,
                needs_android_id: false,
                needs_cpu_unmount: false,
            };
            if key.starts_with("PACKAGES_") {
                let dev_key = format!("{key}_DEVICE");
                if let Some(dev_obj) = root.get(&dev_key).and_then(|v| v.as_object()) {
                    populate_device_info_from_json(dev_obj, &mut group_info, &mut group_flags);
                }
            }
            for item in arr {
                let Some(raw_entry) = item.as_str() else {
                    continue;
                };
                let pkg_name = Self::extract_base_package(raw_entry);
                let with_cpu = raw_entry
                    .split_once(':')
                    .is_some_and(|(_, s)| s == "with_cpu" || s == "cpu");
                if pkg_name.contains('.')
                    && !pkg_name.contains('/')
                    && self.is_package_allowed(pkg_name)
                {
                    new_targets.insert(pkg_name.to_string());
                    if let Some((_, existing_flags)) = new_configs.get_mut(pkg_name) {
                        if with_cpu {
                            existing_flags.needs_cpu_mount = true;
                        }
                    } else {
                        let mut item_flags = group_flags;
                        if with_cpu {
                            item_flags.needs_cpu_mount = true;
                        }
                        new_configs.insert(pkg_name.to_string(), (group_info.clone(), item_flags));
                    }
                }
            }
        }

        if let Some(cs) = root.get("cpu_spoof").and_then(|v| v.as_object()) {
            if let Some(arr) = cs.get("blacklist").and_then(|v| v.as_array()) {
                for item in arr {
                    if let Some(s) = item.as_str() {
                        if !s.is_empty() {
                            new_blacklist.insert(Self::extract_base_package(s).to_string());
                        }
                    }
                }
            }
            if let Some(arr) = cs.get("cpu_only_packages").and_then(|v| v.as_array()) {
                for item in arr {
                    if let Some(s) = item.as_str() {
                        if !s.is_empty() {
                            let pkg_name = Self::extract_base_package(s);
                            if pkg_name.contains('.')
                                && !pkg_name.contains('/')
                                && self.is_package_allowed(pkg_name)
                            {
                                new_cpu_only.insert(pkg_name.to_string());
                                new_targets.insert(pkg_name.to_string());
                                if let Some((_, existing_flags)) = new_configs.get_mut(pkg_name) {
                                    existing_flags.needs_cpu_mount = true;
                                } else {
                                    let cpu_flags = CopgPackageFlags {
                                        needs_cpu_mount: true,
                                        ..Default::default()
                                    };
                                    new_configs.insert(
                                        pkg_name.to_string(),
                                        (CopgDeviceInfo::default(), cpu_flags),
                                    );
                                }
                            }
                        }
                    }
                }
            }
        }

        for blk in &new_blacklist {
            if let Some((_, flags)) = new_configs.get_mut(blk) {
                flags.needs_cpu_mount = false;
                flags.needs_cpu_unmount = true;
            }
        }

        st.target_packages = new_targets;
        st.package_configs = new_configs;
        st.cpu_blacklist = new_blacklist;
        st.cpu_only_packages = new_cpu_only;
        st.config_snapshot = snapshot;
        log_info!(
            "COPG reloaded configuration: {} target packages ({} cpu-only, {} cpu-blacklisted)",
            st.target_packages.len(),
            st.cpu_only_packages.len(),
            st.cpu_blacklist.len()
        );
        true
    }

    fn execute_companion_command(&mut self, command: &str) -> bool {
        let mut result = -1;
        let api_ref: Option<&dyn CompanionConnector> =
            self.api.as_ref().map(|a| a as &dyn CompanionConnector);
        if !self
            .companion_session
            .request(api_ref, command, &mut result)
        {
            log_error!("COPG: Companion command '{command}' transport failed");
            return false;
        }
        log_info!("COPG: Companion command '{command}' completed with code {result}");
        result == 0
    }

    fn install_plt_property_hooks(&mut self, process_name: &str) {
        let Some(api) = self.api else {
            return;
        };
        #[cfg(target_os = "android")]
        {
            let Ok(maps) = fs::read_to_string("/proc/self/maps") else {
                log_warn!("COPG: Cannot open /proc/self/maps for PLT hook scan in {process_name}");
                return;
            };
            let mut libraries_discovered = 0usize;
            let mut hooks_attempted = 0usize;
            let mut hooks_succeeded = 0usize;
            let mut hooked_inodes: BTreeSet<(libc::dev_t, u64)> = BTreeSet::new();

            for line in maps.lines() {
                let mut parts = line.split_whitespace();
                let Some(_range) = parts.next() else { continue };
                let Some(perms) = parts.next() else { continue };
                let Some(_offset) = parts.next() else {
                    continue;
                };
                let Some(dev_str) = parts.next() else {
                    continue;
                };
                let Some(inode_str) = parts.next() else {
                    continue;
                };
                let Some(path) = parts.next() else { continue };

                let inode: u64 = inode_str.parse().unwrap_or(0);
                if inode == 0 || !perms.as_bytes().get(2).is_some_and(|&c| c == b'x') {
                    continue;
                }
                if !path.contains(".so") {
                    continue;
                }
                libraries_discovered += 1;
                if path.contains("libc.so")
                    || path.contains("libm.so")
                    || path.contains("libdl.so")
                    || path.contains("liblog.so")
                    || path.contains("libandroid_runtime.so")
                {
                    continue;
                }
                let Some((maj_s, min_s)) = dev_str.split_once(':') else {
                    continue;
                };
                let Ok(maj) = u32::from_str_radix(maj_s, 16) else {
                    continue;
                };
                let Ok(min) = u32::from_str_radix(min_s, 16) else {
                    continue;
                };
                let dev = unsafe { libc::makedev(maj, min) };
                if hooked_inodes.insert((dev, inode)) {
                    hooks_attempted += 1;
                    unsafe {
                        let mut orig: *mut c_void = std::ptr::null_mut();
                        #[cfg(any(target_arch = "arm", target_arch = "aarch64"))]
                        api.plt_hook_register(
                            dev,
                            inode as libc::ino_t,
                            "__system_property_get",
                            my_copg_system_property_get as *mut c_void,
                            &mut orig,
                        );
                        if !orig.is_null() {
                            ORIG_PLT_PROP_GET.store(orig, Ordering::Release);
                        }
                    }
                    hooks_succeeded += 1;
                }
            }

            if hooks_attempted > 0 {
                self.plt_commit_succeeded = api.plt_hook_commit();
                log_info!(
                    "COPG PLT Hook Status: discovered={libraries_discovered}, attempted={hooks_attempted}, registered={hooks_succeeded}, committed={} for {process_name}",
                    if self.plt_commit_succeeded { 1 } else { 0 }
                );
            } else {
                self.plt_commit_succeeded = false;
                log_info!(
                    "COPG: Discovered {libraries_discovered} mapped libraries in {process_name}, awaiting late-load libc hook coverage"
                );
            }
            self.libraries_discovered = libraries_discovered;
            self.hooks_attempted = hooks_attempted;
            self.hooks_registered = hooks_succeeded;
        }
        #[cfg(not(target_os = "android"))]
        {
            let _ = (api, process_name, fs::read_to_string::<&str>);
            self.libraries_discovered = 0;
            self.hooks_attempted = 0;
            self.hooks_registered = 0;
            self.plt_commit_succeeded = false;
            log_debug!("COPG: [Host Mock] PLT hook installation skipped (not Android)");
        }
    }

    fn install_late_load_property_hook(&mut self) {
        if self.late_hook_installed {
            return;
        }
        #[cfg(all(
            target_os = "android",
            any(target_arch = "arm", target_arch = "aarch64")
        ))]
        unsafe {
            let sh_ret = shadowhook_init(SHADOWHOOK_MODE_SHARED, false);
            if sh_ret != 0 && sh_ret != SHADOWHOOK_ERRNO_PENDING {
                log_warn!(
                    "COPG: shadowhook_init returned {sh_ret} (errno {})",
                    shadowhook_get_errno()
                );
            }
            let sym_get_c = CString::new("__system_property_get").unwrap();
            let sym_get = libc::dlsym(libc::RTLD_DEFAULT, sym_get_c.as_ptr());
            if !sym_get.is_null() {
                let mut orig: *mut c_void = std::ptr::null_mut();
                let h = shadowhook_hook_sym_addr(
                    sym_get,
                    copg_shadow_property_get as *mut c_void,
                    &mut orig,
                );
                if !h.is_null() {
                    ORIG_LIBC_PROP_GET.store(orig, Ordering::Release);
                    self.late_hook_installed = true;
                }
            }
            let sym_cb_c = CString::new("__system_property_read_callback").unwrap();
            let sym_cb = libc::dlsym(libc::RTLD_DEFAULT, sym_cb_c.as_ptr());
            if !sym_cb.is_null() {
                let mut orig_cb: *mut c_void = std::ptr::null_mut();
                let h = shadowhook_hook_sym_addr(
                    sym_cb,
                    copg_shadow_read_callback as *mut c_void,
                    &mut orig_cb,
                );
                if !h.is_null() {
                    ORIG_LIBC_READ_CB.store(orig_cb, Ordering::Release);
                    self.late_hook_installed = true;
                }
            }
            if self.late_hook_installed {
                log_info!(
                    "COPG: Installed Bionic libc ShadowHook property coverage for late-loaded native libraries"
                );
            }
        }
        #[cfg(not(all(
            target_os = "android",
            any(target_arch = "arm", target_arch = "aarch64")
        )))]
        {
            let _ = (
                shadowhook_init,
                shadowhook_get_errno,
                shadowhook_hook_sym_addr,
                SHADOWHOOK_ERRNO_PENDING,
                SHADOWHOOK_MODE_SHARED,
                CStr::from_ptr,
            );
        }
    }

    unsafe fn forge_android_id(&self, fake_id: &str) -> bool {
        if self.env.is_null() || fake_id.is_empty() {
            return false;
        }
        let env = self.env;
        let sec_cls = find_class(env, "android/provider/Settings$Secure");
        exception_check_and_clear(env);
        let nvc_cls = find_class(env, "android/provider/Settings$NameValueCache");
        exception_check_and_clear(env);
        let gt_cls = find_class(env, "android/provider/Settings$GenerationTracker");
        exception_check_and_clear(env);
        let mia_cls = find_class(env, "android/util/MemoryIntArray");
        exception_check_and_clear(env);
        if sec_cls.is_null() || nvc_cls.is_null() || gt_cls.is_null() || mia_cls.is_null() {
            return false;
        }

        let cache_fld = get_static_field_id(
            env,
            sec_cls,
            "sNameValueCache",
            "Landroid/provider/Settings$NameValueCache;",
        );
        exception_check_and_clear(env);
        let values_fld = get_field_id(env, nvc_cls, "mValues", "Landroid/util/ArrayMap;");
        exception_check_and_clear(env);
        let tracks_fld = get_field_id(
            env,
            nvc_cls,
            "mGenerationTrackers",
            "Landroid/util/ArrayMap;",
        );
        exception_check_and_clear(env);
        if cache_fld.is_null() || values_fld.is_null() || tracks_fld.is_null() {
            return false;
        }

        let cache = (jni_vtbl(env).GetStaticObjectField)(env, sec_cls, cache_fld);
        exception_check_and_clear(env);
        if cache.is_null() {
            return false;
        }
        let values = (jni_vtbl(env).GetObjectField)(env, cache, values_fld);
        exception_check_and_clear(env);
        let tracks = (jni_vtbl(env).GetObjectField)(env, cache, tracks_fld);
        exception_check_and_clear(env);
        if values.is_null() || tracks.is_null() {
            return false;
        }

        let mia_ctor = get_method_id(env, mia_cls, "<init>", "(I)V");
        exception_check_and_clear(env);
        let mia_set = get_method_id(env, mia_cls, "set", "(II)V");
        exception_check_and_clear(env);
        let mia: jobject = if !mia_ctor.is_null() {
            (jni_vtbl(env).NewObject)(env, mia_cls, mia_ctor, 1 as jint)
        } else {
            std::ptr::null_mut()
        };
        exception_check_and_clear(env);
        if mia.is_null() {
            return false;
        }
        if !mia_set.is_null() {
            (jni_vtbl(env).CallVoidMethod)(env, mia, mia_set, 0 as jint, 0 as jint);
            exception_check_and_clear(env);
        }

        let key = new_string_utf(env, "android_id");
        if key.is_null() {
            exception_check_and_clear(env);
            delete_local_ref(env, mia);
            return false;
        }

        let gt_ctor = get_method_id(
            env,
            gt_cls,
            "<init>",
            "(Ljava/lang/String;Landroid/util/MemoryIntArray;IILjava/util/function/Consumer;)V",
        );
        exception_check_and_clear(env);
        let tracker: jobject = if !gt_ctor.is_null() {
            (jni_vtbl(env).NewObject)(
                env,
                gt_cls,
                gt_ctor,
                key,
                mia,
                0 as jint,
                0 as jint,
                std::ptr::null_mut::<c_void>(),
            )
        } else {
            std::ptr::null_mut()
        };
        exception_check_and_clear(env);
        if tracker.is_null() {
            delete_local_ref(env, key);
            delete_local_ref(env, mia);
            return false;
        }

        let am_cls = (jni_vtbl(env).GetObjectClass)(env, values);
        let put_mid = if !am_cls.is_null() {
            get_method_id(
                env,
                am_cls,
                "put",
                "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
            )
        } else {
            std::ptr::null_mut()
        };
        exception_check_and_clear(env);
        delete_local_ref(env, am_cls);
        if put_mid.is_null() {
            delete_local_ref(env, tracker);
            delete_local_ref(env, key);
            delete_local_ref(env, mia);
            return false;
        }

        let fake_str = new_string_utf(env, fake_id);
        if fake_str.is_null() {
            exception_check_and_clear(env);
            delete_local_ref(env, tracker);
            delete_local_ref(env, key);
            delete_local_ref(env, mia);
            return false;
        }

        let mut applied = true;
        let p1 = (jni_vtbl(env).CallObjectMethod)(env, values, put_mid, key, fake_str);
        if exception_check_and_clear(env) {
            applied = false;
        }
        delete_local_ref(env, p1);

        let p2 = (jni_vtbl(env).CallObjectMethod)(env, tracks, put_mid, key, tracker);
        if exception_check_and_clear(env) {
            applied = false;
        }
        delete_local_ref(env, p2);

        delete_local_ref(env, fake_str);
        delete_local_ref(env, key);
        delete_local_ref(env, tracker);
        delete_local_ref(env, mia);
        if applied {
            log_info!("COPG [AID]: forged android_id -> {fake_id} (stealth MemoryIntArray cache poisoning)");
        }
        applied
    }

    unsafe fn apply_build_spoofing(&self, info: &CopgDeviceInfo) -> bool {
        if self.env.is_null() {
            return false;
        }
        let env = self.env;
        let mut fields_applied = 0usize;

        let build_cls = find_class(env, "android/os/Build");
        if !exception_check_and_clear(env) && !build_cls.is_null() {
            for (field, val) in [
                ("MODEL", &info.model),
                ("BRAND", &info.brand),
                ("DEVICE", &info.device),
                ("MANUFACTURER", &info.manufacturer),
                ("FINGERPRINT", &info.fingerprint),
                ("PRODUCT", &info.product),
            ] {
                if set_static_string_field(env, build_cls, field, val) {
                    fields_applied += 1;
                }
            }
            if info.should_spoof_serial
                && set_static_string_field(env, build_cls, "SERIAL", &info.serial)
            {
                fields_applied += 1;
            }
            delete_local_ref(env, build_cls);
        }

        let version_cls = find_class(env, "android/os/Build$VERSION");
        if !exception_check_and_clear(env) && !version_cls.is_null() {
            if !info.release.is_empty()
                && set_static_string_field(env, version_cls, "RELEASE", &info.release)
            {
                fields_applied += 1;
            }
            if info.sdk_int > 0 && set_static_int_field(env, version_cls, "SDK_INT", info.sdk_int) {
                fields_applied += 1;
            }
            delete_local_ref(env, version_cls);
        }
        fields_applied > 0
    }
}

impl Engine for CopgEngine {
    fn name(&self) -> &'static str {
        "copg"
    }

    fn is_enabled(&self) -> bool {
        Path::new(&format!("{}/copg.enabled", self.data_dir)).exists()
    }

    fn should_handle_app(&mut self, process_name: &str, _uid: jint) -> bool {
        if !self.is_package_allowed(process_name) {
            return false;
        }
        if let Some(api) = self.api {
            let mut cs = CompanionSession::new();
            let Some(state) = cs.fetch_engine_state(Some(&api as &dyn CompanionConnector), "copg")
            else {
                return false;
            };
            if !state.enabled {
                return false;
            }
            let _ = self.apply_snapshot(state.snapshot, false);
        } else {
            if !self.is_enabled() {
                return false;
            }
            self.reload_config_if_needed(false);
        }
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        if st.target_packages.contains(process_name) {
            return true;
        }
        if process_name.contains(':') {
            return st
                .target_packages
                .contains(Self::extract_base_package(process_name));
        }
        false
    }

    fn on_load(&mut self, api: Option<ZygiskApi>, env: *mut JNIEnv) {
        self.api = api;
        self.env = env;
        log_debug!("COPG Engine onLoad");
    }

    fn pre_app_specialize(&mut self, _args: *mut AppSpecializeArgs, process_name: &str) {
        self.active_for_current_process = false;
        if !self.is_package_allowed(process_name) {
            return;
        }
        if self.api.is_none() {
            self.reload_config_if_needed(false);
        }

        let found = {
            let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
            let entry = st.package_configs.get(process_name).cloned().or_else(|| {
                if process_name.contains(':') {
                    st.package_configs
                        .get(Self::extract_base_package(process_name))
                        .cloned()
                } else {
                    None
                }
            });
            if entry.is_some() {
                st.applied_snapshot = st.config_snapshot.clone();
            }
            entry
        };

        let Some((info, flags)) = found else {
            return;
        };

        self.active_for_current_process = true;
        self.current_device_info = info;
        self.current_flags = flags;
        COPG_ACTIVE_INSTANCE.store(self as *mut Self, Ordering::Release);

        log_info!("COPG preAppSpecialize for target app: {process_name}");
        let api_ref: Option<&dyn CompanionConnector> =
            self.api.as_ref().map(|a| a as &dyn CompanionConnector);
        self.companion_session.pre_specialize(api_ref);

        if self.current_flags.needs_plt_hook {
            self.install_plt_property_hooks(process_name);
        }
    }

    fn post_app_specialize(&mut self, _args: *const AppSpecializeArgs, process_name: &str) {
        if !self.active_for_current_process {
            self.companion_session.close_session();
            return;
        }
        log_info!(
            "COPG postAppSpecialize: applying game & device profile emulation for {process_name}"
        );

        let build_spoof_applied = if self.current_flags.needs_device_spoof {
            unsafe { self.apply_build_spoofing(&self.current_device_info) }
        } else {
            false
        };

        if self.current_flags.needs_plt_hook {
            self.install_late_load_property_hook();
        }

        let android_id_applied = if self.current_flags.needs_android_id
            && self.current_device_info.should_spoof_android_id
        {
            let aid = self.current_device_info.android_id.clone();
            unsafe { self.forge_android_id(&aid) }
        } else {
            false
        };

        self.cpu_mount_active = false;
        if self.current_flags.needs_cpu_unmount {
            self.execute_companion_command("unmount_spoof");
        } else if self.current_flags.needs_cpu_mount {
            let pid = unsafe { libc::getpid() };
            let cmd = format!("mount_cpu:{pid}:");
            if self.execute_companion_command(&cmd) {
                if let Ok(mi) = fs::read_to_string("/proc/self/mountinfo") {
                    if mi.lines().any(|l| l.contains("/proc/cpuinfo")) {
                        self.cpu_mount_active = true;
                    }
                } else {
                    self.cpu_mount_active = true;
                }
                if self.cpu_mount_active {
                    log_info!("COPG: Target-private CPU mount confirmed active for {process_name}");
                } else {
                    log_warn!(
                        "COPG: CPU mount requested and submitted for {process_name}, but /proc/cpuinfo mount not observed in mountinfo"
                    );
                }
            }
        }

        let applied_snapshot = self
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .applied_snapshot
            .clone();
        let mut ack = RuntimeAck {
            engine: "copg".to_string(),
            process_name: process_name.to_string(),
            ..Default::default()
        };
        let ack_bound = bind_runtime_ack_to_snapshot(&mut ack, &applied_snapshot);
        let any_hook_initialized = self.plt_commit_succeeded
            || self.late_hook_installed
            || build_spoof_applied
            || android_id_applied
            || self.cpu_mount_active;

        let intercepted = self.get_intercepted_count();
        if intercepted > 0 {
            ack.set_event(AckEventType::HookObserved);
            ack.init_result = 0;
            ack.detail =
                format!("COPG property queries intercepted and spoofed ({intercepted} calls)");
        } else if any_hook_initialized {
            ack.set_event(AckEventType::HookInitialized);
            ack.init_result = 0;
            ack.detail = format!(
                "COPG hooks initialized (discovered={}, registered={}, committed={}, late_libc={}, build={}, aid={}, cpu_mount={})",
                self.libraries_discovered,
                self.hooks_registered,
                if self.plt_commit_succeeded { 1 } else { 0 },
                if self.late_hook_installed { 1 } else { 0 },
                if build_spoof_applied { 1 } else { 0 },
                if android_id_applied { 1 } else { 0 },
                if self.cpu_mount_active { 1 } else { 0 }
            );
        } else {
            ack.set_event(AckEventType::ProcessLoaded);
            ack.init_result = 0;
            ack.detail = "COPG targeted process loaded".to_string();
        }

        if !ack_bound {
            log_warn!("COPG: Runtime ACK withheld without current configuration identity");
        } else {
            let mut resp = -1;
            let cmd = format!("ack:{}", ack.to_json_compact());
            let api_ref: Option<&dyn CompanionConnector> =
                self.api.as_ref().map(|a| a as &dyn CompanionConnector);
            if !self.companion_session.request(api_ref, &cmd, &mut resp)
                && unsafe { libc::geteuid() } == 0
            {
                write_runtime_ack_for_current_config(&self.data_dir, &ack);
            }
        }

        self.companion_session.close_session();

        if !self.current_flags.needs_plt_hook && !self.late_hook_installed {
            if let Some(api) = self.api {
                api.set_option(ZygiskOption::DlcloseModuleLibrary);
                log_debug!("COPG: DLCLOSE_MODULE_LIBRARY requested for stealth");
            }
        }
    }
}
