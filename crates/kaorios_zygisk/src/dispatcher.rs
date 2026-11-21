// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use crate::engine::Engine;
use crate::jni_sys::{get_string_utf, jstring, JNIEnv};
use crate::zygisk_abi::{
    AppSpecializeArgs, ServerSpecializeArgs, ZygiskApi, ZygiskOption, ZYGISK_API_VERSION,
};
use kaorios_common::{log_debug, log_error, log_info};
use std::collections::HashSet;
use std::panic::{catch_unwind, AssertUnwindSafe};

pub struct KaoriosDispatcher {
    api: Option<ZygiskApi>,
    env: *mut JNIEnv,
    engines: Vec<Box<dyn Engine>>,
    active_app_indices: Vec<usize>,
    active_server_indices: Vec<usize>,
    failed_indices: HashSet<usize>,
    handling_server: bool,
}

unsafe impl Send for KaoriosDispatcher {}

impl Default for KaoriosDispatcher {
    fn default() -> Self {
        Self::new()
    }
}

impl KaoriosDispatcher {
    pub fn new() -> Self {
        Self {
            api: None,
            env: std::ptr::null_mut(),
            engines: Vec::new(),
            active_app_indices: Vec::new(),
            active_server_indices: Vec::new(),
            failed_indices: HashSet::new(),
            handling_server: false,
        }
    }

    pub fn register_engine(&mut self, engine: Box<dyn Engine>) {
        log_info!("Registering engine: {}", engine.name());
        self.engines.push(engine);
    }

    fn record_failure(&mut self, idx: usize) {
        self.failed_indices.insert(idx);
        log_error!(
            "Engine {} lifecycle failed; isolated for this process",
            self.engines[idx].name()
        );
    }

    unsafe fn resolve_app_process_name(
        env: *mut JNIEnv,
        args: *const AppSpecializeArgs,
    ) -> String {
        if args.is_null() {
            return String::new();
        }
        let nice_ptr = (*args).nice_name;
        let nice: jstring = if nice_ptr.is_null() {
            std::ptr::null_mut()
        } else {
            *nice_ptr
        };
        let mut name = get_string_utf(env, nice);
        if name.is_empty() {
            let dir_ptr = (*args).app_data_dir;
            let dir_jstr: jstring = if dir_ptr.is_null() {
                std::ptr::null_mut()
            } else {
                *dir_ptr
            };
            let data_dir = get_string_utf(env, dir_jstr);
            if let Some(pos) = data_dir.rfind('/') {
                if pos + 1 < data_dir.len() {
                    name = data_dir[pos + 1..].to_string();
                }
            }
        }
        name
    }

    pub fn on_load(&mut self, api: Option<ZygiskApi>, env: *mut JNIEnv) {
        self.api = api;
        self.env = env;
        log_info!("KaoriosHelper Zygisk Dispatcher loaded (API v{ZYGISK_API_VERSION})");

        for i in 0..self.engines.len() {
            if self.failed_indices.contains(&i) {
                continue;
            }
            let ok = catch_unwind(AssertUnwindSafe(|| {
                self.engines[i].on_load(api, env);
            }))
            .is_ok();
            if !ok {
                self.record_failure(i);
            }
        }
    }

    pub fn pre_app_specialize(&mut self, args: *mut AppSpecializeArgs) {
        self.active_app_indices.clear();
        let process_name = unsafe { Self::resolve_app_process_name(self.env, args) };
        let uid = unsafe {
            if !args.is_null() && !(*args).uid.is_null() {
                *(*args).uid
            } else {
                0
            }
        };

        for i in 0..self.engines.len() {
            if self.failed_indices.contains(&i) {
                continue;
            }
            let res = catch_unwind(AssertUnwindSafe(|| {
                self.engines[i].should_handle_app(&process_name, uid)
            }));
            match res {
                Ok(true) => self.active_app_indices.push(i),
                Ok(false) => {}
                Err(_) => self.record_failure(i),
            }
        }

        if self.active_app_indices.is_empty() {
            if let Some(api) = self.api {
                api.set_option(ZygiskOption::DlcloseModuleLibrary);
            }
            return;
        }

        log_debug!(
            "Process '{process_name}' claimed by {} engine(s)",
            self.active_app_indices.len()
        );
        for idx in self.active_app_indices.clone() {
            let ok = catch_unwind(AssertUnwindSafe(|| {
                self.engines[idx].pre_app_specialize(args, &process_name);
            }))
            .is_ok();
            if !ok {
                self.record_failure(idx);
            }
        }
    }

    pub fn post_app_specialize(&mut self, args: *const AppSpecializeArgs) {
        if self.active_app_indices.is_empty() {
            return;
        }
        let process_name = unsafe { Self::resolve_app_process_name(self.env, args) };
        for idx in self.active_app_indices.clone() {
            if self.failed_indices.contains(&idx) {
                continue;
            }
            let ok = catch_unwind(AssertUnwindSafe(|| {
                self.engines[idx].post_app_specialize(args, &process_name);
            }))
            .is_ok();
            if !ok {
                self.record_failure(idx);
            }
        }
    }

    pub fn pre_server_specialize(&mut self, args: *mut ServerSpecializeArgs) {
        self.handling_server = false;
        self.active_server_indices.clear();

        for i in 0..self.engines.len() {
            if self.failed_indices.contains(&i) {
                continue;
            }
            let res = catch_unwind(AssertUnwindSafe(|| {
                if self.engines[i].should_handle_server() {
                    self.engines[i].pre_server_specialize(args);
                    true
                } else {
                    false
                }
            }));
            match res {
                Ok(true) => {
                    self.active_server_indices.push(i);
                    self.handling_server = true;
                }
                Ok(false) => {}
                Err(_) => self.record_failure(i),
            }
        }

        if !self.handling_server {
            if let Some(api) = self.api {
                api.set_option(ZygiskOption::DlcloseModuleLibrary);
            }
        }
    }

    pub fn post_server_specialize(&mut self, args: *const ServerSpecializeArgs) {
        if !self.handling_server {
            return;
        }
        for idx in self.active_server_indices.clone() {
            if self.failed_indices.contains(&idx) {
                continue;
            }
            let ok = catch_unwind(AssertUnwindSafe(|| {
                self.engines[idx].post_server_specialize(args);
            }))
            .is_ok();
            if !ok {
                self.record_failure(idx);
            }
        }
    }
}
