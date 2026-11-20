// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use crate::engine::Engine;
use crate::jni_sys::{
    delete_local_ref, exception_check_and_clear, find_class, get_method_id, get_static_method_id,
    jclass, jint, jni_vtbl, new_string_utf, set_static_int_field, set_static_string_field, JNIEnv,
};
use crate::shadowhook::{
    shadowhook_get_errno, shadowhook_hook_sym_addr, shadowhook_init, SHADOWHOOK_ERRNO_PENDING,
    SHADOWHOOK_MODE_UNIQUE,
};
use crate::zygisk_abi::{AppSpecializeArgs, ZygiskApi, ZygiskOption};
use kaorios_common::ack_protocol::{AckEventType, RuntimeAck};
use kaorios_common::companion_ipc::{CompanionConnector, CompanionSession};
use kaorios_common::runtime_snapshot::{
    bind_runtime_ack_to_snapshot, read_runtime_config_snapshot,
    write_runtime_ack_for_current_config, RuntimeConfigSnapshot,
};
#[cfg_attr(not(target_os = "android"), allow(unused_imports))]
use kaorios_common::{log_debug, log_error, log_info, log_warn, DEFAULT_DATA_DIR};
use serde_json::{Map, Value};
use std::collections::BTreeMap;
#[cfg_attr(not(target_os = "android"), allow(unused_imports))]
use std::ffi::{CStr, CString};
#[cfg_attr(not(target_os = "android"), allow(unused_imports))]
use std::os::raw::{c_char, c_int, c_void};
use std::path::Path;
use std::sync::atomic::{AtomicPtr, AtomicU64, Ordering};
use std::sync::Mutex;

static ACTIVE_SPOOF_PROPS: Mutex<BTreeMap<String, String>> = Mutex::new(BTreeMap::new());
static PROPERTIES_INTERCEPTED: AtomicU64 = AtomicU64::new(0);

#[cfg(all(target_os = "android", any(target_arch = "arm", target_arch = "aarch64")))]
const PROP_VALUE_MAX: usize = 92;

#[cfg(all(target_os = "android", any(target_arch = "arm", target_arch = "aarch64")))]
type PropCallback = unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, u32);

#[cfg(all(target_os = "android", any(target_arch = "arm", target_arch = "aarch64")))]
#[repr(C)]
struct PropertyCallbackContext {
    original_callback: PropCallback,
    original_cookie: *mut c_void,
}

#[cfg_attr(
    not(all(target_os = "android", any(target_arch = "arm", target_arch = "aarch64"))),
    allow(dead_code)
)]
static ORIG_PROP_READ_CB: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
#[cfg_attr(
    not(all(target_os = "android", any(target_arch = "arm", target_arch = "aarch64"))),
    allow(dead_code)
)]
static ORIG_PROP_GET: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());

#[cfg(all(target_os = "android", any(target_arch = "arm", target_arch = "aarch64")))]
unsafe extern "C" fn pif_modify_callback(
    cookie: *mut c_void,
    name: *const c_char,
    value: *const c_char,
    serial: u32,
) {
    if cookie.is_null() || name.is_null() || value.is_null() {
        return;
    }
    let ctx = &*(cookie as *const PropertyCallbackContext);
    let name_str = CStr::from_ptr(name).to_string_lossy();
    let val_str = CStr::from_ptr(value).to_string_lossy();
    if let Some(spoofed) = PifEngine::resolve_spoofed_property(&name_str, &val_str) {
        PROPERTIES_INTERCEPTED.fetch_add(1, Ordering::Relaxed);
        if let Ok(c_spoof) = CString::new(spoofed.as_str()) {
            log_debug!("PIF [ShadowHook libc] [{name_str}]: {val_str} -> {spoofed}");
            (ctx.original_callback)(ctx.original_cookie, name, c_spoof.as_ptr(), serial);
            return;
        }
    }
    (ctx.original_callback)(ctx.original_cookie, name, value, serial);
}

#[cfg(all(target_os = "android", any(target_arch = "arm", target_arch = "aarch64")))]
unsafe extern "C" fn my_system_property_read_callback(
    pi: *const c_void,
    callback: Option<PropCallback>,
    cookie: *mut c_void,
) {
    let orig_ptr = ORIG_PROP_READ_CB.load(Ordering::Acquire);
    let Some(cb) = callback else {
        return;
    };
    if orig_ptr.is_null() || pi.is_null() {
        return;
    }
    let orig: unsafe extern "C" fn(*const c_void, PropCallback, *mut c_void) =
        std::mem::transmute(orig_ptr);
    let mut ctx = PropertyCallbackContext {
        original_callback: cb,
        original_cookie: cookie,
    };
    orig(pi, pif_modify_callback, (&mut ctx as *mut PropertyCallbackContext).cast());
}

#[cfg(all(target_os = "android", any(target_arch = "arm", target_arch = "aarch64")))]
unsafe extern "C" fn my_system_property_get(name: *const c_char, value: *mut c_char) -> c_int {
    if name.is_null() || value.is_null() {
        return 0;
    }
    let name_str = CStr::from_ptr(name).to_string_lossy();
    if let Some(spoofed) = PifEngine::resolve_spoofed_property(&name_str, "") {
        PROPERTIES_INTERCEPTED.fetch_add(1, Ordering::Relaxed);
        let bytes = spoofed.as_bytes();
        let len = bytes.len().min(PROP_VALUE_MAX - 1);
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), value.cast(), len);
        *value.add(len) = 0;
        log_debug!("PIF [ShadowHook libc get] [{name_str}] -> {spoofed}");
        return len as c_int;
    }
    let orig_ptr = ORIG_PROP_GET.load(Ordering::Acquire);
    if !orig_ptr.is_null() {
        let orig: unsafe extern "C" fn(*const c_char, *mut c_char) -> c_int =
            std::mem::transmute(orig_ptr);
        return orig(name, value);
    }
    0
}

fn parse_pif_flag_int(val: &Value, fallback: i32) -> i32 {
    if let Some(b) = val.as_bool() {
        return if b { 1 } else { 0 };
    }
    if let Some(i) = val.as_i64() {
        return i as i32;
    }
    if let Some(f) = val.as_f64() {
        return f as i32;
    }
    if let Some(s) = val.as_str() {
        let norm = s.trim().to_ascii_lowercase();
        if norm.is_empty() {
            return fallback;
        }
        return match norm.as_str() {
            "1" | "true" => 1,
            "0" | "false" => 0,
            _ => norm.parse::<i32>().unwrap_or(0),
        };
    }
    fallback
}

pub struct PifEngine {
    data_dir: String,
    api: Option<ZygiskApi>,
    env: *mut JNIEnv,
    companion_session: CompanionSession,
    config_loaded: bool,
    hook_installed: bool,
    provider_installed: bool,
    pif_dex_bytes: Vec<u8>,
    cached_props: BTreeMap<String, String>,
    last_valid_props: BTreeMap<String, String>,
    cached_spoof_provider: i32,
    last_valid_spoof_provider: i32,
    device_initial_sdk_int: i32,
    last_valid_device_initial_sdk_int: i32,
    spoof_props: i32,
    last_valid_spoof_props: i32,
    spoof_build: i32,
    last_valid_spoof_build: i32,
    config_snapshot: RuntimeConfigSnapshot,
    last_valid_snapshot: RuntimeConfigSnapshot,
}

unsafe impl Send for PifEngine {}

impl Default for PifEngine {
    fn default() -> Self {
        Self::new(DEFAULT_DATA_DIR.to_string())
    }
}

impl PifEngine {
    pub fn new(data_dir: String) -> Self {
        Self {
            data_dir,
            api: None,
            env: std::ptr::null_mut(),
            companion_session: CompanionSession::new(),
            config_loaded: false,
            hook_installed: false,
            provider_installed: false,
            pif_dex_bytes: Vec::new(),
            cached_props: BTreeMap::new(),
            last_valid_props: BTreeMap::new(),
            cached_spoof_provider: 0,
            last_valid_spoof_provider: 0,
            device_initial_sdk_int: 0,
            last_valid_device_initial_sdk_int: 0,
            spoof_props: 1,
            last_valid_spoof_props: 1,
            spoof_build: 1,
            last_valid_spoof_build: 1,
            config_snapshot: RuntimeConfigSnapshot::default(),
            last_valid_snapshot: RuntimeConfigSnapshot::default(),
        }
    }

    pub fn is_tee_active(&self) -> bool {
        Path::new(&format!("{}/tee.runtime_active", self.data_dir)).exists()
    }

    pub fn is_hook_installed(&self) -> bool {
        self.hook_installed
    }

    pub fn is_provider_installed(&self) -> bool {
        self.provider_installed
    }

    pub fn get_effective_spoof_provider(&self) -> i32 {
        self.cached_spoof_provider
    }

    pub fn get_spoofed_properties(&self) -> &BTreeMap<String, String> {
        &self.cached_props
    }

    pub fn resolve_spoofed_property(name: &str, _orig_value: &str) -> Option<String> {
        let props = ACTIVE_SPOOF_PROPS.lock().unwrap_or_else(|e| e.into_inner());
        if props.is_empty() {
            return None;
        }
        if let Some(v) = props.get(name) {
            return Some(v.clone());
        }
        for (pattern, val) in props.iter() {
            if pattern.len() > 1 && pattern.starts_with('*') {
                let suffix = &pattern[1..];
                if name.ends_with(suffix) {
                    return Some(val.clone());
                }
            } else if pattern.len() > 1 && pattern.ends_with('*') {
                let prefix = &pattern[..pattern.len() - 1];
                if name.starts_with(prefix) {
                    return Some(val.clone());
                }
            }
        }
        None
    }

    fn restore_last_known_good(
        &mut self,
        props: &mut BTreeMap<String, String>,
        spoof_provider: &mut i32,
    ) -> bool {
        if self.last_valid_props.is_empty() {
            props.clear();
            return false;
        }
        log_info!("PIF: Retaining last-known-good in-memory configuration");
        *props = self.last_valid_props.clone();
        *spoof_provider = if self.is_tee_active() {
            0
        } else {
            self.last_valid_spoof_provider
        };
        self.device_initial_sdk_int = self.last_valid_device_initial_sdk_int;
        self.spoof_props = self.last_valid_spoof_props;
        self.spoof_build = self.last_valid_spoof_build;
        self.config_snapshot = self.last_valid_snapshot.clone();
        *ACTIVE_SPOOF_PROPS.lock().unwrap_or_else(|e| e.into_inner()) = props.clone();
        true
    }

    pub fn load_config(
        &mut self,
        props: &mut BTreeMap<String, String>,
        spoof_provider: &mut i32,
    ) -> bool {
        *spoof_provider = 0;
        self.spoof_props = 1;
        self.spoof_build = 1;
        self.device_initial_sdk_int = 0;

        let Ok(snapshot) = read_runtime_config_snapshot(&self.data_dir, "pif", "pif.json") else {
            log_warn!("PIF: Configuration snapshot unavailable");
            return self.restore_last_known_good(props, spoof_provider);
        };
        if snapshot.bytes.is_empty() {
            props.clear();
            return false;
        }

        let trimmed = snapshot.bytes.trim();
        let root_obj: Map<String, Value> =
            if !trimmed.is_empty() && !trimmed.starts_with('{') && !trimmed.starts_with('[') {
                let mut map = Map::new();
                for line in trimmed.lines() {
                    let line = line.trim();
                    if line.is_empty() || line.starts_with('#') {
                        continue;
                    }
                    let Some(eq) = line.find('=') else {
                        log_warn!("PIF: Invalid pif.prop syntax in {}", self.data_dir);
                        return self.restore_last_known_good(props, spoof_provider);
                    };
                    if eq == 0 {
                        log_warn!("PIF: Invalid pif.prop syntax in {}", self.data_dir);
                        return self.restore_last_known_good(props, spoof_provider);
                    }
                    let k = line[..eq].trim();
                    let mut v = line[eq + 1..].trim();
                    if let Some(comment) = v.find('#') {
                        v = v[..comment].trim();
                    }
                    if k.is_empty() || v.is_empty() {
                        log_warn!("PIF: Empty key or value in pif.prop");
                        return self.restore_last_known_good(props, spoof_provider);
                    }
                    map.insert(k.to_string(), Value::String(v.to_string()));
                }
                map
            } else {
                match serde_json::from_str::<Value>(&snapshot.bytes) {
                    Ok(Value::Object(m)) => m,
                    Ok(_) => {
                        log_warn!("PIF: JSON root is not an object in {}/pif.json", self.data_dir);
                        return self.restore_last_known_good(props, spoof_provider);
                    }
                    Err(e) => {
                        log_warn!("PIF: Failed to parse {}/pif.json as JSON ({e})", self.data_dir);
                        return self.restore_last_known_good(props, spoof_provider);
                    }
                }
            };

        if root_obj.is_empty()
            || root_obj
                .get("enabled")
                .and_then(|v| v.as_bool())
                == Some(false)
        {
            log_info!("PIF: Configuration is empty or explicitly disabled");
            props.clear();
            ACTIVE_SPOOF_PROPS
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .clear();
            return false;
        }

        props.clear();
        for (key, val) in &root_obj {
            if key.starts_with("//") {
                continue;
            }
            if key == "spoofProvider" {
                *spoof_provider = parse_pif_flag_int(val, 0);
            } else if key == "spoofProps" {
                self.spoof_props = parse_pif_flag_int(val, 1);
            } else if key == "spoofBuild" {
                self.spoof_build = parse_pif_flag_int(val, 1);
            } else if matches!(
                key.as_str(),
                "spoofSignature"
                    | "enabled"
                    | "spoofGms"
                    | "spoofVending"
                    | "spoofVendingSdk"
                    | "spoofVendingFinger"
            ) {
                continue;
            } else if matches!(
                key.as_str(),
                "DEVICE_INITIAL_SDK_INT" | "VERSION.DEVICE_INITIAL_SDK_INT" | "FIRST_API_LEVEL"
            ) {
                if let Some(i) = val.as_i64() {
                    self.device_initial_sdk_int = i as i32;
                } else if let Some(s) = val.as_str() {
                    self.device_initial_sdk_int = s.trim().parse::<i32>().unwrap_or(0);
                }
            } else if let Some(s) = val.as_str() {
                props.insert(key.clone(), s.to_string());
            } else if let Some(i) = val.as_i64() {
                props.insert(key.clone(), i.to_string());
            } else if let Some(b) = val.as_bool() {
                props.insert(key.clone(), if b { "1" } else { "0" }.to_string());
            }
        }

        if !props.contains_key("RELEASE") {
            if let Some(v) = props.get("VERSION.RELEASE").cloned() {
                props.insert("RELEASE".to_string(), v);
            }
        }
        if !props.contains_key("INCREMENTAL") {
            if let Some(v) = props.get("VERSION.INCREMENTAL").cloned() {
                props.insert("INCREMENTAL".to_string(), v);
            }
        }
        if !props.contains_key("SECURITY_PATCH") {
            if let Some(v) = props.get("VERSION.SECURITY_PATCH").cloned() {
                props.insert("SECURITY_PATCH".to_string(), v);
            }
        }
        if let Some(sp) = props.get_mut("SECURITY_PATCH") {
            if sp.len() == 8 && sp.bytes().all(|b| b.is_ascii_digit()) {
                *sp = format!("{}-{}-{}", &sp[0..4], &sp[4..6], &sp[6..8]);
            }
        }
        if self.device_initial_sdk_int <= 0 {
            for sdk_key in ["*api_level", "*.first_api_level"] {
                if let Some(v) = props.get(sdk_key) {
                    if let Ok(parsed) = v.trim().parse::<i32>() {
                        if parsed > 0 {
                            self.device_initial_sdk_int = parsed;
                            break;
                        }
                    }
                }
            }
        }

        let map_field = [
            ("MODEL", "ro.product.model"),
            ("MANUFACTURER", "ro.product.manufacturer"),
            ("BRAND", "ro.product.brand"),
            ("DEVICE", "ro.product.device"),
            ("PRODUCT", "ro.product.name"),
            ("BOARD", "ro.product.board"),
            ("HARDWARE", "ro.hardware"),
            ("FINGERPRINT", "ro.build.fingerprint"),
            ("ID", "ro.build.id"),
            ("TYPE", "ro.build.type"),
            ("TAGS", "ro.build.tags"),
            ("RELEASE", "ro.build.version.release"),
            ("INCREMENTAL", "ro.build.version.incremental"),
        ];
        for (src, dst) in map_field {
            if let Some(v) = props.get(src).cloned() {
                props.insert(dst.to_string(), v);
            }
        }
        if let Some(sp) = props.get("SECURITY_PATCH").cloned() {
            props.insert("ro.build.version.security_patch".to_string(), sp.clone());
            props.insert("*.security_patch".to_string(), sp);
        }
        if self.device_initial_sdk_int > 0 {
            let sdk_str = self.device_initial_sdk_int.to_string();
            props.insert("ro.product.first_api_level".to_string(), sdk_str.clone());
            props.insert("*first_api_level".to_string(), sdk_str.clone());
            props.entry("*api_level".to_string()).or_insert(sdk_str);
        }

        props.insert("ro.boot.flash.locked".to_string(), "1".to_string());
        props.insert("ro.boot.verifiedbootstate".to_string(), "green".to_string());
        props.insert("ro.boot.vbmeta.device_state".to_string(), "locked".to_string());

        if self.is_tee_active() && *spoof_provider != 0 {
            log_warn!(
                "PIF: TEE Engine is active! Forcing spoofProvider = 0 to prevent KeyStore collision."
            );
            *spoof_provider = 0;
        }

        self.last_valid_props = props.clone();
        self.last_valid_spoof_provider = *spoof_provider;
        self.last_valid_device_initial_sdk_int = self.device_initial_sdk_int;
        self.last_valid_spoof_props = self.spoof_props;
        self.last_valid_spoof_build = self.spoof_build;
        self.last_valid_snapshot = snapshot.clone();

        *ACTIVE_SPOOF_PROPS.lock().unwrap_or_else(|e| e.into_inner()) = props.clone();
        self.config_snapshot = snapshot;
        true
    }

    fn install_property_hook(&mut self) {
        if self.hook_installed {
            return;
        }
        #[cfg(all(target_os = "android", any(target_arch = "arm", target_arch = "aarch64")))]
        unsafe {
            let sh_ret = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
            if sh_ret != 0 && sh_ret != SHADOWHOOK_ERRNO_PENDING {
                let err = shadowhook_get_errno();
                log_warn!("PIF: shadowhook_init returned {sh_ret} (errno {err})");
            }

            let sym_cb_name = CString::new("__system_property_read_callback").unwrap();
            let sym_addr = libc::dlsym(libc::RTLD_DEFAULT, sym_cb_name.as_ptr());
            if !sym_addr.is_null() {
                let mut orig: *mut c_void = std::ptr::null_mut();
                let handle = shadowhook_hook_sym_addr(
                    sym_addr,
                    my_system_property_read_callback as *mut c_void,
                    &mut orig,
                );
                if !handle.is_null() {
                    ORIG_PROP_READ_CB.store(orig, Ordering::Release);
                    self.hook_installed = true;
                    log_info!(
                        "PIF: ShadowHook successfully intercepted __system_property_read_callback"
                    );
                } else {
                    log_error!(
                        "PIF: Failed to hook __system_property_read_callback. ShadowHook errno: {}",
                        shadowhook_get_errno()
                    );
                }
            } else {
                log_error!("PIF: Could not resolve __system_property_read_callback via dlsym");
            }

            let sym_get_name = CString::new("__system_property_get").unwrap();
            let sym_get = libc::dlsym(libc::RTLD_DEFAULT, sym_get_name.as_ptr());
            if !sym_get.is_null() {
                let mut orig_get: *mut c_void = std::ptr::null_mut();
                let handle_get = shadowhook_hook_sym_addr(
                    sym_get,
                    my_system_property_get as *mut c_void,
                    &mut orig_get,
                );
                if !handle_get.is_null() {
                    ORIG_PROP_GET.store(orig_get, Ordering::Release);
                    self.hook_installed = true;
                    log_info!("PIF: ShadowHook successfully intercepted __system_property_get");
                } else {
                    log_warn!(
                        "PIF: Failed to hook __system_property_get. ShadowHook errno: {}",
                        shadowhook_get_errno()
                    );
                }
            }
        }

        #[cfg(all(
            target_os = "android",
            not(any(target_arch = "arm", target_arch = "aarch64"))
        ))]
        {
            log_warn!(
                "PIF: Non-ARM architecture detected. KaoriosHelper is built exclusively for ARM devices (arm64-v8a, armeabi-v7a)."
            );
            self.hook_installed = false;
        }

        #[cfg(not(target_os = "android"))]
        {
            let _ = (
                shadowhook_init,
                shadowhook_get_errno,
                shadowhook_hook_sym_addr,
                SHADOWHOOK_ERRNO_PENDING,
                SHADOWHOOK_MODE_UNIQUE,
                CStr::from_ptr,
            );
            self.hook_installed = true;
            log_info!("PIF: [Mock] Native Bionic libc property hook registered");
        }
    }

    fn preload_dex_payload(&mut self) {
        self.pif_dex_bytes.clear();
        unsafe {
            let mut fd = -1;
            if let Some(api) = self.api {
                let mod_dir = api.get_module_dir();
                if mod_dir >= 0 {
                    let c_dex = CString::new("classes.dex").unwrap();
                    fd = libc::openat(
                        mod_dir,
                        c_dex.as_ptr(),
                        libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                    );
                    libc::close(mod_dir);
                }
            }
            if fd < 0 {
                let fallback = CString::new(format!("{}/classes.dex", self.data_dir)).unwrap();
                fd = libc::open(
                    fallback.as_ptr(),
                    libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                );
            }
            if fd < 0 {
                return;
            }
            let mut st: libc::stat = std::mem::zeroed();
            if libc::fstat(fd, &mut st) == 0
                && (st.st_mode & libc::S_IFMT) == libc::S_IFREG
                && st.st_size > 0
                && (st.st_size as u64) <= 16 * 1024 * 1024
            {
                let size = st.st_size as usize;
                let mut buf = vec![0u8; size];
                let mut offset = 0usize;
                while offset < size {
                    let n = libc::read(fd, buf[offset..].as_mut_ptr().cast(), size - offset);
                    if n < 0 {
                        let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                        if err == libc::EINTR {
                            continue;
                        }
                        buf.clear();
                        break;
                    }
                    if n == 0 {
                        buf.clear();
                        break;
                    }
                    offset += n as usize;
                }
                self.pif_dex_bytes = buf;
            }
            libc::close(fd);
        }
    }

    unsafe fn try_install_custom_provider(&mut self) -> bool {
        if self.env.is_null()
            || self.pif_dex_bytes.is_empty()
            || self.cached_spoof_provider <= 0
            || self.is_tee_active()
        {
            return false;
        }
        let env = self.env;
        let cl_class = find_class(env, "java/lang/ClassLoader");
        if exception_check_and_clear(env) || cl_class.is_null() {
            return false;
        }
        let get_sys = get_static_method_id(
            env,
            cl_class,
            "getSystemClassLoader",
            "()Ljava/lang/ClassLoader;",
        );
        if exception_check_and_clear(env) || get_sys.is_null() {
            delete_local_ref(env, cl_class);
            return false;
        }
        let sys_loader = (jni_vtbl(env).CallStaticObjectMethod)(env, cl_class, get_sys);
        if exception_check_and_clear(env) || sys_loader.is_null() {
            delete_local_ref(env, cl_class);
            return false;
        }
        let byte_buf_cls = find_class(env, "java/nio/ByteBuffer");
        let in_mem_cls = find_class(env, "dalvik/system/InMemoryDexClassLoader");
        if exception_check_and_clear(env) || byte_buf_cls.is_null() || in_mem_cls.is_null() {
            delete_local_ref(env, byte_buf_cls);
            delete_local_ref(env, in_mem_cls);
            delete_local_ref(env, sys_loader);
            delete_local_ref(env, cl_class);
            return false;
        }

        let direct_buf = (jni_vtbl(env).NewDirectByteBuffer)(
            env,
            self.pif_dex_bytes.as_mut_ptr().cast(),
            self.pif_dex_bytes.len() as i64,
        );
        let buf_array = if !direct_buf.is_null() {
            (jni_vtbl(env).NewObjectArray)(env, 1, byte_buf_cls, direct_buf)
        } else {
            std::ptr::null_mut()
        };
        let in_mem_init = get_method_id(
            env,
            in_mem_cls,
            "<init>",
            "([Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V",
        );
        let in_mem_loader = if !buf_array.is_null() && !in_mem_init.is_null() {
            (jni_vtbl(env).NewObject)(env, in_mem_cls, in_mem_init, buf_array, sys_loader)
        } else {
            std::ptr::null_mut()
        };
        let load_class_mid = get_method_id(
            env,
            cl_class,
            "loadClass",
            "(Ljava/lang/String;)Ljava/lang/Class;",
        );

        let mut installed = false;
        if !exception_check_and_clear(env)
            && !in_mem_loader.is_null()
            && !load_class_mid.is_null()
        {
            let entry_name = new_string_utf(env, "es.chiteroman.playintegrityfix.EntryPoint");
            let entry_cls: jclass = if !entry_name.is_null() {
                (jni_vtbl(env).CallObjectMethod)(env, in_mem_loader, load_class_mid, entry_name)
            } else {
                std::ptr::null_mut()
            };
            delete_local_ref(env, entry_name);
            if !exception_check_and_clear(env) && !entry_cls.is_null() {
                let init_mid = get_static_method_id(env, entry_cls, "initProvider", "()Z");
                if !exception_check_and_clear(env) && !init_mid.is_null() {
                    let ok = (jni_vtbl(env).CallStaticBooleanMethod)(env, entry_cls, init_mid);
                    if !exception_check_and_clear(env) {
                        installed = ok != 0;
                    }
                } else {
                    exception_check_and_clear(env);
                }
                delete_local_ref(env, entry_cls);
            }
        } else {
            exception_check_and_clear(env);
        }

        delete_local_ref(env, in_mem_loader);
        delete_local_ref(env, buf_array);
        delete_local_ref(env, direct_buf);
        delete_local_ref(env, in_mem_cls);
        delete_local_ref(env, byte_buf_cls);
        delete_local_ref(env, sys_loader);
        delete_local_ref(env, cl_class);
        installed
    }
}

impl Engine for PifEngine {
    fn name(&self) -> &'static str {
        "pif"
    }

    fn is_enabled(&self) -> bool {
        !Path::new(&format!("{}/pif.disabled", self.data_dir)).exists()
    }

    fn should_handle_app(&mut self, process_name: &str, _uid: jint) -> bool {
        if !self.is_enabled() {
            return false;
        }
        matches!(
            process_name,
            "com.google.android.gms" | "com.google.android.gms.unstable" | "com.android.vending"
        )
    }

    fn on_load(&mut self, api: Option<ZygiskApi>, env: *mut JNIEnv) {
        self.api = api;
        self.env = env;
        log_debug!("PIF Engine onLoad");
    }

    fn pre_app_specialize(&mut self, _args: *mut AppSpecializeArgs, process_name: &str) {
        log_info!("PIF targeting process: {process_name}");
        self.provider_installed = false;
        let mut props = std::mem::take(&mut self.cached_props);
        let mut sp = self.cached_spoof_provider;
        self.config_loaded = self.load_config(&mut props, &mut sp);
        self.cached_props = props;
        self.cached_spoof_provider = sp;

        if self.config_loaded && self.cached_spoof_provider > 0 && !self.is_tee_active() {
            self.preload_dex_payload();
        } else {
            self.pif_dex_bytes.clear();
        }
        let api_ref: Option<&dyn CompanionConnector> = self
            .api
            .as_ref()
            .map(|a| a as &dyn CompanionConnector);
        self.companion_session.pre_specialize(api_ref);

        if let Some(api) = self.api {
            api.set_option(ZygiskOption::ForceDenylistUnmount);
        }
    }

    fn post_app_specialize(&mut self, _args: *const AppSpecializeArgs, process_name: &str) {
        if !self.config_loaded {
            let mut props = std::mem::take(&mut self.cached_props);
            let mut sp = self.cached_spoof_provider;
            self.config_loaded = self.load_config(&mut props, &mut sp);
            self.cached_props = props;
            self.cached_spoof_provider = sp;
        }

        if !self.config_loaded || self.cached_props.is_empty() {
            log_warn!("PIF: No valid configuration found to apply");
            self.companion_session.close_session();
            return;
        }

        if self.spoof_props > 0 {
            self.install_property_hook();
        }

        if self.spoof_build > 0 && !self.env.is_null() {
            unsafe {
                let env = self.env;
                let build_cls = find_class(env, "android/os/Build");
                if !exception_check_and_clear(env) && !build_cls.is_null() {
                    for field in [
                        "MANUFACTURER",
                        "MODEL",
                        "BRAND",
                        "DEVICE",
                        "PRODUCT",
                        "FINGERPRINT",
                        "ID",
                        "TYPE",
                        "TAGS",
                        "BOARD",
                        "HARDWARE",
                    ] {
                        if let Some(val) = self.cached_props.get(field) {
                            set_static_string_field(env, build_cls, field, val);
                        }
                    }
                    delete_local_ref(env, build_cls);
                }

                let version_cls = find_class(env, "android/os/Build$VERSION");
                if !exception_check_and_clear(env) && !version_cls.is_null() {
                    for field in ["RELEASE", "INCREMENTAL", "SECURITY_PATCH"] {
                        if let Some(val) = self.cached_props.get(field) {
                            set_static_string_field(env, version_cls, field, val);
                        }
                    }
                    if self.device_initial_sdk_int > 0 {
                        set_static_int_field(
                            env,
                            version_cls,
                            "DEVICE_INITIAL_SDK_INT",
                            self.device_initial_sdk_int,
                        );
                    }
                    delete_local_ref(env, version_cls);
                }
            }
        }

        if self.cached_spoof_provider > 0 && !self.is_tee_active() {
            self.provider_installed = unsafe { self.try_install_custom_provider() };
            if self.provider_installed {
                log_info!("PIF: Installed Java CustomProvider for {process_name}");
            }
        } else {
            self.provider_installed = false;
            self.pif_dex_bytes.clear();
        }

        log_info!("PIF properties and native hooks specialization completed in {process_name}");

        let mut ack = RuntimeAck {
            engine: "pif".to_string(),
            process_name: process_name.to_string(),
            ..Default::default()
        };
        if !bind_runtime_ack_to_snapshot(&mut ack, &self.config_snapshot) {
            log_warn!("PIF: Runtime ACK withheld without current configuration identity");
            self.companion_session.close_session();
            return;
        }

        if self.hook_installed
            || (self.spoof_props == 0 && (self.spoof_build > 0 || self.provider_installed))
        {
            ack.init_result = 0;
            if PROPERTIES_INTERCEPTED.load(Ordering::Relaxed) > 0 {
                ack.set_event(AckEventType::HookObserved);
            } else {
                ack.set_event(AckEventType::HookInitialized);
            }
            ack.detail = if self.hook_installed {
                if self.provider_installed {
                    "ShadowHook Bionic libc __system_property_read_callback + Java CustomProvider active"
                } else {
                    "ShadowHook Bionic libc __system_property_read_callback active"
                }
            } else if self.provider_installed {
                "Java Build spoofing + CustomProvider active (spoofProps disabled)"
            } else {
                "Java Build spoofing active (spoofProps disabled)"
            }
            .to_string();
        } else {
            ack.init_result = -1;
            ack.error_code = libc::ENOSYS;
            ack.set_event(AckEventType::ApplyFailed);
            #[cfg(not(any(target_arch = "arm", target_arch = "aarch64")))]
            {
                ack.detail = "Architecture unsupported: KaoriosHelper is built exclusively for ARM devices (arm64-v8a, armeabi-v7a)".to_string();
            }
            #[cfg(any(target_arch = "arm", target_arch = "aarch64"))]
            {
                ack.detail = "ShadowHook hook failed or unsupported on this ABI".to_string();
            }
        }

        let mut resp = -1;
        let cmd = format!("ack:{}", ack.to_json_compact());
        let api_ref: Option<&dyn CompanionConnector> = self
            .api
            .as_ref()
            .map(|a| a as &dyn CompanionConnector);
        if !self.companion_session.request(api_ref, &cmd, &mut resp)
            && unsafe { libc::geteuid() } == 0
        {
            write_runtime_ack_for_current_config(&self.data_dir, &ack);
        }
        self.companion_session.close_session();
    }
}
