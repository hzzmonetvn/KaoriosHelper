// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project
#![allow(
    clippy::duplicate_mod,
    clippy::missing_safety_doc,
    clippy::not_unsafe_ptr_arg_deref
)]

pub mod dispatcher;
pub mod engine;
pub mod engines;
pub mod jni_sys;
pub mod shadowhook;
pub mod zygisk_abi;

use crate::dispatcher::KaoriosDispatcher;
use crate::engines::copg::CopgEngine;
use crate::engines::hma::HmaEngine;
use crate::engines::pif::PifEngine;
use crate::engines::tee::TeeEngine;
use crate::jni_sys::JNIEnv;
use crate::zygisk_abi::{
    ApiTable, AppSpecializeArgs, ModuleAbi, ServerSpecializeArgs, ZygiskApi, ZYGISK_API_VERSION,
};
use kaorios_common::companion_server::handle_companion_session;
use std::os::raw::{c_int, c_void};
use std::sync::Mutex;

static DISPATCHER: Mutex<Option<KaoriosDispatcher>> = Mutex::new(None);

unsafe extern "C" fn pre_app_specialize(impl_: *mut c_void, args: *mut AppSpecializeArgs) {
    let lock = &*(impl_ as *const Mutex<Option<KaoriosDispatcher>>);
    if let Ok(mut guard) = lock.lock() {
        if let Some(d) = guard.as_mut() {
            d.pre_app_specialize(args);
        }
    }
}

unsafe extern "C" fn post_app_specialize(impl_: *mut c_void, args: *const AppSpecializeArgs) {
    let lock = &*(impl_ as *const Mutex<Option<KaoriosDispatcher>>);
    if let Ok(mut guard) = lock.lock() {
        if let Some(d) = guard.as_mut() {
            d.post_app_specialize(args);
        }
    }
}

unsafe extern "C" fn pre_server_specialize(impl_: *mut c_void, args: *mut ServerSpecializeArgs) {
    let lock = &*(impl_ as *const Mutex<Option<KaoriosDispatcher>>);
    if let Ok(mut guard) = lock.lock() {
        if let Some(d) = guard.as_mut() {
            d.pre_server_specialize(args);
        }
    }
}

unsafe extern "C" fn post_server_specialize(
    impl_: *mut c_void,
    args: *const ServerSpecializeArgs,
) {
    let lock = &*(impl_ as *const Mutex<Option<KaoriosDispatcher>>);
    if let Ok(mut guard) = lock.lock() {
        if let Some(d) = guard.as_mut() {
            d.post_server_specialize(args);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn zygisk_module_entry(table: *mut ApiTable, env: *mut JNIEnv) {
    let Some(api) = ZygiskApi::from_raw(table) else {
        return;
    };

    let mut dispatcher = KaoriosDispatcher::new();
    dispatcher.register_engine(Box::new(HmaEngine::default()));
    dispatcher.register_engine(Box::new(PifEngine::default()));
    dispatcher.register_engine(Box::new(TeeEngine::default()));
    dispatcher.register_engine(Box::new(CopgEngine::default()));

    if let Ok(mut guard) = DISPATCHER.lock() {
        *guard = Some(dispatcher);
    }

    static mut MODULE_ABI: ModuleAbi = ModuleAbi {
        api_version: ZYGISK_API_VERSION,
        impl_: std::ptr::null_mut(),
        pre_app_specialize,
        post_app_specialize,
        pre_server_specialize,
        post_server_specialize,
    };

    let abi_ptr = std::ptr::addr_of_mut!(MODULE_ABI);
    (*abi_ptr).impl_ = (&DISPATCHER as *const Mutex<Option<KaoriosDispatcher>>)
        .cast_mut()
        .cast();

    let Some(reg_fn) = (*table).register_module else {
        return;
    };
    if !reg_fn(table, abi_ptr) {
        return;
    }

    if let Ok(mut guard) = DISPATCHER.lock() {
        if let Some(d) = guard.as_mut() {
            d.on_load(Some(api), env);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn zygisk_companion_entry(client: c_int) {
    handle_companion_session(client);
}
