// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use crate::jni_sys::{jint, JNIEnv};
use crate::zygisk_abi::{AppSpecializeArgs, ServerSpecializeArgs, ZygiskApi};

pub trait Engine: Send {
    fn name(&self) -> &'static str;
    fn is_enabled(&self) -> bool;

    fn on_load(&mut self, _api: Option<ZygiskApi>, _env: *mut JNIEnv) {}

    fn should_handle_app(&mut self, _process_name: &str, _uid: jint) -> bool {
        false
    }

    fn should_handle_server(&mut self) -> bool {
        false
    }

    fn pre_app_specialize(&mut self, _args: *mut AppSpecializeArgs, _process_name: &str) {}
    fn post_app_specialize(&mut self, _args: *const AppSpecializeArgs, _process_name: &str) {}

    fn pre_server_specialize(&mut self, _args: *mut ServerSpecializeArgs) {}
    fn post_server_specialize(&mut self, _args: *const ServerSpecializeArgs) {}
}
