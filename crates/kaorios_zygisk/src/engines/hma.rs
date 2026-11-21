// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use crate::engine::Engine;
use crate::jni_sys::{
    delete_local_ref, exception_check_and_clear, find_class, get_method_id, get_static_method_id,
    get_string_utf, jclass, jint, jni_vtbl, jobject, jstring, new_string_utf, JNIEnv,
};
use crate::zygisk_abi::{JNINativeMethod, ServerSpecializeArgs, ZygiskApi};
use kaorios_common::ack_protocol::{AckEventType, RuntimeAck};
use kaorios_common::companion_ipc::{CompanionConnector, CompanionSession};
use kaorios_common::runtime_snapshot::{
    bind_runtime_ack_to_snapshot, read_runtime_config_snapshot,
    write_runtime_ack_for_current_config, RuntimeConfigSnapshot,
};
use kaorios_common::{log_debug, log_error, log_info, log_warn, DEFAULT_DATA_DIR};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::CString;
use std::os::raw::c_void;
use std::path::Path;
use std::sync::atomic::{AtomicPtr, AtomicU64, Ordering};
use std::sync::Mutex;

static ACTIVE_HMA_ENGINE: AtomicPtr<HmaEngine> = AtomicPtr::new(std::ptr::null_mut());

fn expand_hma_preset(preset: &str, ignored: &BTreeSet<String>, out: &mut BTreeSet<String>) {
    let norm = preset.to_ascii_lowercase();
    let add_root = matches!(
        norm.as_str(),
        "root" | "magisk" | "kernelsu" | "apatch" | "default" | "all"
    );
    let add_xposed = matches!(norm.as_str(), "xposed" | "lsposed" | "default" | "all");
    let add_hma = matches!(
        norm.as_str(),
        "hma" | "detectors" | "devtools" | "default" | "all"
    );

    let mut add_group = |pkgs: &[&str]| {
        for &p in pkgs {
            if !ignored.contains(p) {
                out.insert(p.to_string());
            }
        }
    };
    if add_root {
        add_group(&[
            "com.topjohnwu.magisk",
            "io.github.vvb2060.magisk",
            "io.github.huskydg.magisk",
            "me.weishu.kernelsu",
            "com.rifsxd.ksunext",
            "io.github.a13e300.ksu",
            "org.apatch",
            "me.bmax.apatch",
            "eu.chainfire.supersu",
            "com.koushikdutta.superuser",
        ]);
    }
    if add_xposed {
        add_group(&[
            "org.lsposed.manager",
            "de.robv.android.xposed.installer",
            "org.meowcat.edxposed.manager",
            "rikka.sui",
        ]);
    }
    if add_hma {
        add_group(&[
            "com.tsng.hidemyapplist",
            "org.frknkrc44.hma_oss",
            "rikka.shizuku.app",
            "moe.shizuku.privileged.api",
            "bin.mt.plus",
        ]);
    }
}

unsafe extern "C" fn jni_on_hook_state_changed(
    env: *mut JNIEnv,
    _cls: jclass,
    state: jint,
    j_detail: jstring,
) {
    let ptr = ACTIVE_HMA_ENGINE.load(Ordering::Acquire);
    if ptr.is_null() {
        return;
    }
    let detail = if !env.is_null() && !j_detail.is_null() {
        let s = get_string_utf(env, j_detail);
        if s.is_empty() {
            "HMA async state update".to_string()
        } else {
            s
        }
    } else {
        "HMA async state update".to_string()
    };
    (&*ptr).on_java_hook_state_changed(state, &detail);
}

#[derive(Default)]
struct HmaState {
    config_loaded: bool,
    config_snapshot: RuntimeConfigSnapshot,
    applied_snapshot: RuntimeConfigSnapshot,
    scope_map: BTreeMap<String, BTreeSet<String>>,
    whitelist_map: BTreeMap<String, BTreeSet<String>>,
    opposite_exempt_map: BTreeMap<String, BTreeSet<String>>,
    global_hidden_packages: BTreeSet<String>,
}

pub struct HmaEngine {
    data_dir: String,
    api: Option<ZygiskApi>,
    env: *mut JNIEnv,
    dex_entrypoint: jobject,
    dex_mmap_addr: *mut c_void,
    dex_mmap_size: usize,
    companion_session: Mutex<CompanionSession>,
    state: Mutex<HmaState>,
    ack_mutex: Mutex<()>,
    filter_count: AtomicU64,
}

unsafe impl Send for HmaEngine {}
unsafe impl Sync for HmaEngine {}

impl Default for HmaEngine {
    fn default() -> Self {
        Self::new(DEFAULT_DATA_DIR.to_string())
    }
}

impl Drop for HmaEngine {
    fn drop(&mut self) {
        let _ = ACTIVE_HMA_ENGINE.compare_exchange(
            self as *mut Self,
            std::ptr::null_mut(),
            Ordering::AcqRel,
            Ordering::Relaxed,
        );
        self.cleanup_dex();
    }
}

impl HmaEngine {
    pub fn new(data_dir: String) -> Self {
        Self {
            data_dir,
            api: None,
            env: std::ptr::null_mut(),
            dex_entrypoint: std::ptr::null_mut(),
            dex_mmap_addr: std::ptr::null_mut(),
            dex_mmap_size: 0,
            companion_session: Mutex::new(CompanionSession::new()),
            state: Mutex::new(HmaState::default()),
            ack_mutex: Mutex::new(()),
            filter_count: AtomicU64::new(0),
        }
    }

    fn cleanup_dex(&mut self) {
        if let Ok(mut cs) = self.companion_session.lock() {
            cs.close_session();
        }
        if !self.dex_entrypoint.is_null() && !self.env.is_null() {
            unsafe {
                (jni_vtbl(self.env).DeleteGlobalRef)(self.env, self.dex_entrypoint);
            }
            self.dex_entrypoint = std::ptr::null_mut();
        }
        if !self.dex_mmap_addr.is_null() && self.dex_mmap_size > 0 {
            unsafe {
                libc::munmap(self.dex_mmap_addr, self.dex_mmap_size);
            }
            self.dex_mmap_addr = std::ptr::null_mut();
            self.dex_mmap_size = 0;
        }
    }

    pub fn load_config(&self) -> bool {
        let Ok(snapshot) = read_runtime_config_snapshot(&self.data_dir, "hma", "hma.json") else {
            log_warn!("HMA: Configuration snapshot unavailable");
            return false;
        };
        self.load_config_from_snapshot(snapshot)
    }

    pub fn load_config_from_snapshot(&self, snapshot: RuntimeConfigSnapshot) -> bool {
        if snapshot.bytes.is_empty() {
            return false;
        }
        let Ok(j) = serde_json::from_str::<Value>(&snapshot.bytes) else {
            log_error!("HMA: Failed to parse {}/hma.json as valid JSON", self.data_dir);
            return false;
        };
        let Some(root) = j.as_object() else {
            log_error!("HMA: Failed to parse {}/hma.json as valid JSON", self.data_dir);
            return false;
        };

        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        st.scope_map.clear();
        st.whitelist_map.clear();
        st.opposite_exempt_map.clear();
        st.global_hidden_packages.clear();

        let use_defaults = root
            .get("useDefaultGlobalHidden")
            .and_then(|v| v.as_bool())
            .unwrap_or(true);
        if use_defaults {
            for p in [
                "com.topjohnwu.magisk",
                "io.github.vvb2060.magisk",
                "me.weishu.kernelsu",
                "io.github.a13e300.ksu",
                "org.apatch",
                "me.bmax.apatch",
                "org.lsposed.manager",
            ] {
                st.global_hidden_packages.insert(p.to_string());
            }
        }

        for gh_key in ["global_hidden", "globalHidden"] {
            if let Some(arr) = root.get(gh_key).and_then(|v| v.as_array()) {
                for pkg in arr {
                    if let Some(s) = pkg.as_str() {
                        if !s.is_empty() {
                            st.global_hidden_packages.insert(s.to_string());
                        }
                    }
                }
            }
        }

        let mut ignored_preset_pkgs = BTreeSet::new();
        if let Some(arr) = root
            .get("ignoredPackagesForPresets")
            .and_then(|v| v.as_array())
        {
            for pkg in arr {
                if let Some(s) = pkg.as_str() {
                    if !s.is_empty() {
                        ignored_preset_pkgs.insert(s.to_string());
                    }
                }
            }
        }

        let mut templates: BTreeMap<String, BTreeSet<String>> = BTreeMap::new();
        let mut template_whitelist: BTreeMap<String, bool> = BTreeMap::new();
        if let Some(tpl_obj) = root.get("templates").and_then(|v| v.as_object()) {
            for (tpl_name, tpl_val) in tpl_obj {
                if let Some(arr) = tpl_val.as_array() {
                    let set = templates.entry(tpl_name.clone()).or_default();
                    for pkg in arr {
                        if let Some(s) = pkg.as_str() {
                            set.insert(s.to_string());
                        }
                    }
                    template_whitelist.insert(tpl_name.clone(), false);
                } else if let Some(obj) = tpl_val.as_object() {
                    let set = templates.entry(tpl_name.clone()).or_default();
                    if let Some(arr) = obj.get("appList").and_then(|v| v.as_array()) {
                        for pkg in arr {
                            if let Some(s) = pkg.as_str() {
                                set.insert(s.to_string());
                            }
                        }
                    }
                    let is_wl = obj
                        .get("isWhitelist")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false);
                    template_whitelist.insert(tpl_name.clone(), is_wl);
                }
            }
        }

        if let Some(wl_obj) = root.get("whitelist").and_then(|v| v.as_object()) {
            for (caller, list_val) in wl_obj {
                if let Some(arr) = list_val.as_array() {
                    let allowed: BTreeSet<String> = arr
                        .iter()
                        .filter_map(|v| v.as_str().map(String::from))
                        .collect();
                    st.whitelist_map.insert(caller.clone(), allowed);
                }
            }
        }

        if let Some(scope_obj) = root.get("scope").and_then(|v| v.as_object()) {
            for (caller, val) in scope_obj {
                if let Some(arr) = val.as_array() {
                    let hidden: BTreeSet<String> = arr
                        .iter()
                        .filter_map(|v| v.as_str().map(String::from))
                        .collect();
                    st.scope_map.insert(caller.clone(), hidden);
                } else if let Some(rule) = val.as_object() {
                    let mut is_wl = rule
                        .get("useWhitelist")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false);
                    let mut targets = BTreeSet::new();
                    if let Some(arr) = rule.get("extraAppList").and_then(|v| v.as_array()) {
                        for pkg in arr {
                            if let Some(s) = pkg.as_str() {
                                targets.insert(s.to_string());
                            }
                        }
                    }
                    if let Some(arr) = rule.get("applyTemplates").and_then(|v| v.as_array()) {
                        for tpl in arr {
                            if let Some(tname) = tpl.as_str() {
                                if let Some(tset) = templates.get(tname) {
                                    targets.extend(tset.iter().cloned());
                                }
                                if template_whitelist.get(tname).copied().unwrap_or(false) {
                                    is_wl = true;
                                }
                            }
                        }
                    }
                    if let Some(arr) = rule.get("applyPresets").and_then(|v| v.as_array()) {
                        for preset in arr {
                            if let Some(s) = preset.as_str() {
                                if !s.is_empty() {
                                    expand_hma_preset(s, &ignored_preset_pkgs, &mut targets);
                                }
                            }
                        }
                    }
                    let mut opposites = BTreeSet::new();
                    if let Some(arr) = rule
                        .get("extraOppositeAppList")
                        .and_then(|v| v.as_array())
                    {
                        for pkg in arr {
                            if let Some(s) = pkg.as_str() {
                                if !s.is_empty() {
                                    opposites.insert(s.to_string());
                                }
                            }
                        }
                    }
                    for op in &opposites {
                        targets.remove(op);
                    }
                    if is_wl {
                        st.whitelist_map.insert(caller.clone(), targets);
                        if !opposites.is_empty() {
                            st.scope_map
                                .entry(caller.clone())
                                .or_default()
                                .extend(opposites);
                        }
                    } else {
                        st.scope_map.insert(caller.clone(), targets);
                        if !opposites.is_empty() {
                            st.opposite_exempt_map.insert(caller.clone(), opposites);
                        }
                    }
                }
            }
        }

        let sc_len = st.scope_map.len();
        let wl_len = st.whitelist_map.len();
        let gh_len = st.global_hidden_packages.len();
        st.config_snapshot = snapshot;
        st.config_loaded = true;
        log_info!(
            "HMA config loaded: {sc_len} scoped callers, {wl_len} whitelists, {gh_len} global hidden packages"
        );
        true
    }

    pub fn reload_config_if_needed(&self) -> bool {
        let Ok(snapshot) = read_runtime_config_snapshot(&self.data_dir, "hma", "hma.json") else {
            return false;
        };
        {
            let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
            if st.config_loaded && st.config_snapshot.same_identity(&snapshot) {
                return true;
            }
        }
        self.load_config_from_snapshot(snapshot)
    }

    pub fn should_hide(&self, calling_uid: i32, calling_pkgs: &[&str], target_pkg: &str) -> bool {
        if calling_uid < 10000 || target_pkg.is_empty() {
            return false;
        }
        if calling_pkgs.contains(&target_pkg) {
            return false;
        }
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        if !st.config_loaded {
            return false;
        }
        for &caller in calling_pkgs {
            if st
                .opposite_exempt_map
                .get(caller)
                .is_some_and(|set| set.contains(target_pkg))
            {
                return false;
            }
        }
        if st.global_hidden_packages.contains(target_pkg) {
            self.filter_count.fetch_add(1, Ordering::Relaxed);
            return true;
        }
        for &caller in calling_pkgs {
            if let Some(wl) = st.whitelist_map.get(caller) {
                if !wl.is_empty() && !wl.contains(target_pkg) {
                    self.filter_count.fetch_add(1, Ordering::Relaxed);
                    return true;
                }
            }
            if st
                .scope_map
                .get(caller)
                .is_some_and(|set| set.contains(target_pkg))
            {
                self.filter_count.fetch_add(1, Ordering::Relaxed);
                return true;
            }
        }
        false
    }

    pub fn get_filtered_count(&self) -> u64 {
        self.filter_count.load(Ordering::Relaxed)
    }

    pub fn get_scope_size(&self) -> usize {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).scope_map.len()
    }

    pub fn get_whitelist_size(&self) -> usize {
        self.state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .whitelist_map
            .len()
    }

    unsafe fn try_load_dex(&mut self, module_dir: libc::c_int) -> bool {
        if self.env.is_null() {
            return false;
        }
        let env = self.env;
        let c_dex = CString::new("classes.dex").unwrap();
        let mut fd = -1;
        if module_dir >= 0 {
            fd = libc::openat(
                module_dir,
                c_dex.as_ptr(),
                libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            );
        }
        if fd < 0 {
            let fallback = CString::new(format!("{}/classes.dex", self.data_dir)).unwrap();
            fd = libc::open(
                fallback.as_ptr(),
                libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            );
        }
        if fd < 0 {
            log_debug!("HMA: No classes.dex found in module or data dir; skipping DEX injection");
            return false;
        }

        let mut st: libc::stat = std::mem::zeroed();
        if libc::fstat(fd, &mut st) < 0
            || !kaorios_common::is_regular_file(st.st_mode)
            || st.st_size <= 0
        {
            libc::close(fd);
            return false;
        }
        let size = st.st_size as usize;
        let data = libc::mmap(
            std::ptr::null_mut(),
            size,
            libc::PROT_READ,
            libc::MAP_SHARED,
            fd,
            0,
        );
        libc::close(fd);
        if data == libc::MAP_FAILED {
            log_error!("HMA: Failed to mmap DEX file");
            return false;
        }

        let cl_class = find_class(env, "java/lang/ClassLoader");
        if exception_check_and_clear(env) || cl_class.is_null() {
            libc::munmap(data, size);
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
            libc::munmap(data, size);
            return false;
        }
        let sys_loader = (jni_vtbl(env).CallStaticObjectMethod)(env, cl_class, get_sys);
        if exception_check_and_clear(env) || sys_loader.is_null() {
            delete_local_ref(env, cl_class);
            libc::munmap(data, size);
            return false;
        }

        let byte_buf_cls = find_class(env, "java/nio/ByteBuffer");
        let in_mem_cls = find_class(env, "dalvik/system/InMemoryDexClassLoader");
        let direct_buf = (jni_vtbl(env).NewDirectByteBuffer)(env, data, size as i64);
        let buf_array = if !byte_buf_cls.is_null() && !direct_buf.is_null() {
            (jni_vtbl(env).NewObjectArray)(env, 1, byte_buf_cls, direct_buf)
        } else {
            std::ptr::null_mut()
        };
        let in_mem_init = if !in_mem_cls.is_null() {
            get_method_id(
                env,
                in_mem_cls,
                "<init>",
                "([Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V",
            )
        } else {
            std::ptr::null_mut()
        };
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

        let mut entry_class: jclass = std::ptr::null_mut();
        let mut fallback_class: jclass = std::ptr::null_mut();

        if !exception_check_and_clear(env)
            && !in_mem_loader.is_null()
            && !load_class_mid.is_null()
        {
            for name in [
                "org.frknkrc44.hma_oss.zygote.ZygoteEntry",
                "com.v7878.zygisk.EntryPoint",
            ] {
                let jname = new_string_utf(env, name);
                if jname.is_null() {
                    exception_check_and_clear(env);
                    continue;
                }
                let cand = (jni_vtbl(env).CallObjectMethod)(env, in_mem_loader, load_class_mid, jname);
                delete_local_ref(env, jname);
                if exception_check_and_clear(env) || cand.is_null() {
                    continue;
                }
                let init_check = get_static_method_id(env, cand, "init", "(Ljava/lang/String;)V");
                exception_check_and_clear(env);
                let state_check = get_static_method_id(env, cand, "getHookState", "()I");
                exception_check_and_clear(env);
                if !init_check.is_null() && !state_check.is_null() {
                    entry_class = cand;
                    break;
                }
                if fallback_class.is_null() {
                    fallback_class = cand;
                } else {
                    delete_local_ref(env, cand);
                }
            }
        } else {
            exception_check_and_clear(env);
        }

        if entry_class.is_null() && !fallback_class.is_null() {
            entry_class = fallback_class;
            fallback_class = std::ptr::null_mut();
        }
        if !fallback_class.is_null() {
            delete_local_ref(env, fallback_class);
        }

        if !entry_class.is_null() {
            self.dex_entrypoint = (jni_vtbl(env).NewGlobalRef)(env, entry_class);
            delete_local_ref(env, entry_class);

            let m_name = CString::new("nativeOnHookStateChanged").unwrap();
            let m_sig = CString::new("(ILjava/lang/String;)V").unwrap();
            let method = JNINativeMethod {
                name: m_name.as_ptr(),
                signature: m_sig.as_ptr(),
                fn_ptr: jni_on_hook_state_changed as *mut c_void,
            };
            (jni_vtbl(env).RegisterNatives)(env, self.dex_entrypoint, &method, 1);
            exception_check_and_clear(env);

            self.dex_mmap_addr = data;
            self.dex_mmap_size = size;
            log_info!("HMA: Injected DEX loaded successfully into system_server");
        } else {
            log_warn!("HMA: None of the candidate entrypoints found in classes.dex");
            libc::munmap(data, size);
        }

        delete_local_ref(env, in_mem_loader);
        delete_local_ref(env, buf_array);
        delete_local_ref(env, direct_buf);
        delete_local_ref(env, in_mem_cls);
        delete_local_ref(env, byte_buf_cls);
        delete_local_ref(env, sys_loader);
        delete_local_ref(env, cl_class);

        !self.dex_entrypoint.is_null()
    }

    fn publish_hook_state_ack(
        &self,
        snapshot: &RuntimeConfigSnapshot,
        hook_state: i32,
        detail: &str,
        explicit_failure: bool,
    ) {
        let _guard = self.ack_mutex.lock().unwrap_or_else(|e| e.into_inner());
        let mut ack = RuntimeAck {
            engine: "hma".to_string(),
            process_name: "system_server".to_string(),
            ..Default::default()
        };
        if !bind_runtime_ack_to_snapshot(&mut ack, snapshot) {
            log_warn!("HMA: Runtime ACK withheld without current configuration identity");
            return;
        }
        if explicit_failure || hook_state < 3 {
            ack.init_result = -1;
            ack.error_code = libc::EFAULT;
            ack.set_event(AckEventType::ApplyFailed);
        } else if hook_state >= 8 {
            ack.init_result = 0;
            ack.error_code = 0;
            ack.set_event(AckEventType::HookObserved);
        } else if hook_state >= 5 {
            ack.init_result = 0;
            ack.error_code = 0;
            ack.set_event(AckEventType::HookInitialized);
        } else {
            ack.init_result = 0;
            ack.error_code = 0;
            ack.set_event(AckEventType::HookRegistered);
        }
        ack.detail = detail.to_string();

        let mut resp = -1;
        let cmd = format!("ack:{}", ack.to_json_compact());
        let api_ref: Option<&dyn CompanionConnector> = self
            .api
            .as_ref()
            .map(|a| a as &dyn CompanionConnector);
        if self
            .companion_session
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .request(api_ref, &cmd, &mut resp)
        {
            return;
        }
        if unsafe { libc::geteuid() } == 0 {
            write_runtime_ack_for_current_config(&self.data_dir, &ack);
        }
    }

    pub fn on_java_hook_state_changed(&self, state: i32, detail: &str) {
        let snap = self
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .applied_snapshot
            .clone();
        let failed = state == 4 && detail.contains("Timed out");
        self.publish_hook_state_ack(&snap, state, detail, failed);
        if failed || state >= 8 {
            self.companion_session
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .close_session();
        }
    }
}

impl Engine for HmaEngine {
    fn name(&self) -> &'static str {
        "hma"
    }

    fn is_enabled(&self) -> bool {
        !Path::new(&format!("{}/hma.disabled", self.data_dir)).exists()
    }

    fn should_handle_server(&mut self) -> bool {
        if let Some(api) = self.api {
            let api_ref: &dyn CompanionConnector = &api;
            let mut cs = self.companion_session.lock().unwrap_or_else(|e| e.into_inner());
            if !cs.has_retained_socket() {
                cs.pre_specialize(Some(api_ref));
            }
            let Some(state) = cs.fetch_engine_state(Some(api_ref), "hma") else {
                cs.close_session();
                return false;
            };
            if !state.enabled {
                cs.close_session();
                return false;
            }
            drop(cs);
            if !state.snapshot.bytes.is_empty() {
                let _ = self.load_config_from_snapshot(state.snapshot);
            }
            return true;
        }
        self.is_enabled()
    }

    fn on_load(&mut self, api: Option<ZygiskApi>, env: *mut JNIEnv) {
        self.api = api;
        self.env = env;
        log_debug!("HMA Engine onLoad");
    }

    fn pre_server_specialize(&mut self, _args: *mut ServerSpecializeArgs) {
        log_debug!("HMA preServerSpecialize");
        if self.api.is_none() {
            let _ = self.load_config();
        } else {
            let need_fetch = {
                let cs = self.companion_session.lock().unwrap_or_else(|e| e.into_inner());
                let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
                !cs.has_retained_socket() || !st.config_loaded
            };
            if need_fetch {
                if let Some(api) = self.api {
                    let api_ref: &dyn CompanionConnector = &api;
                    let fetched = {
                        let mut cs = self.companion_session.lock().unwrap_or_else(|e| e.into_inner());
                        if !cs.has_retained_socket() {
                            cs.pre_specialize(Some(api_ref));
                        }
                        cs.fetch_engine_state(Some(api_ref), "hma")
                    };
                    if let Some(state) = fetched {
                        if state.enabled && !state.snapshot.bytes.is_empty() {
                            let _ = self.load_config_from_snapshot(state.snapshot);
                        }
                    }
                }
            }
        }

        if let Some(api) = self.api {
            let mod_dir = api.get_module_dir();
            if mod_dir >= 0 {
                unsafe {
                    self.try_load_dex(mod_dir);
                    libc::close(mod_dir);
                }
            }
        }
    }

    fn post_server_specialize(&mut self, _args: *const ServerSpecializeArgs) {
        log_info!("HMA initializing inside system_server");
        let already_loaded = self
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .config_loaded;
        if !already_loaded && self.api.is_none() {
            self.load_config();
        }
        let snapshot = {
            let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
            st.applied_snapshot = st.config_snapshot.clone();
            st.applied_snapshot.clone()
        };
        ACTIVE_HMA_ENGINE.store(self as *mut Self, Ordering::Release);

        let mut close_companion = false;
        if !self.dex_entrypoint.is_null() && !self.env.is_null() {
            unsafe {
                let env = self.env;
                let init_mid =
                    get_static_method_id(env, self.dex_entrypoint, "init", "(Ljava/lang/String;)V");
                let mut init_invoked = false;
                let mut snapshot_supplied = false;
                let mut init_threw = false;
                if !exception_check_and_clear(env) && !init_mid.is_null() {
                    let j_cfg = new_string_utf(env, &snapshot.bytes);
                    (jni_vtbl(env).CallStaticVoidMethod)(env, self.dex_entrypoint, init_mid, j_cfg);
                    delete_local_ref(env, j_cfg);
                    if exception_check_and_clear(env) {
                        log_error!("HMA: Exception occurred during ZygoteEntry.init(snapshot)");
                        init_threw = true;
                    } else {
                        log_info!("HMA: ZygoteEntry.init(snapshot) executed successfully in system_server");
                        init_invoked = true;
                        snapshot_supplied = true;
                    }
                } else {
                    exception_check_and_clear(env);
                    let main_mid = get_static_method_id(env, self.dex_entrypoint, "main", "()V");
                    if !exception_check_and_clear(env) && !main_mid.is_null() {
                        (jni_vtbl(env).CallStaticVoidMethod)(env, self.dex_entrypoint, main_mid);
                        if exception_check_and_clear(env) {
                            log_error!("HMA: Exception occurred during ZygoteEntry.main()");
                            init_threw = true;
                        } else {
                            log_info!("HMA: ZygoteEntry.main() executed inside system_server (without snapshot binding)");
                            init_invoked = true;
                        }
                    } else {
                        exception_check_and_clear(env);
                    }
                }

                if init_threw {
                    self.publish_hook_state_ack(
                        &snapshot,
                        0,
                        "HMA entrypoint threw unhandled Java exception",
                        true,
                    );
                    close_companion = true;
                } else if init_invoked && snapshot_supplied {
                    let mut hook_state = 3;
                    let get_state_mid =
                        get_static_method_id(env, self.dex_entrypoint, "getHookState", "()I");
                    if !exception_check_and_clear(env) && !get_state_mid.is_null() {
                        hook_state = (jni_vtbl(env).CallStaticIntMethod)(
                            env,
                            self.dex_entrypoint,
                            get_state_mid,
                        );
                        exception_check_and_clear(env);
                    }
                    self.publish_hook_state_ack(
                        &snapshot,
                        hook_state,
                        &format!("HMA framework hooks stage: {hook_state}"),
                        false,
                    );
                    if hook_state >= 8 {
                        close_companion = true;
                    }
                }
            }
        } else {
            close_companion = true;
        }

        log_info!("HMA: In-process PackageManagerService and SettingsProvider protection initialized");
        if close_companion {
            self.companion_session
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .close_session();
        }
        self.api = None;
    }
}
