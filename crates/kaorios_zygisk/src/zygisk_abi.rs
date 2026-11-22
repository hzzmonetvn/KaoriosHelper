// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use crate::jni_sys::{jboolean, jint, jintArray, jlong, jobjectArray, jstring, JNIEnv};
use kaorios_common::companion_ipc::CompanionConnector;
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_long, c_void};

pub const ZYGISK_API_VERSION: c_long = 4;

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ZygiskOption {
    ForceDenylistUnmount = 0,
    DlcloseModuleLibrary = 1,
}

pub const PROCESS_GRANTED_ROOT: u32 = 1 << 0;
pub const PROCESS_ON_DENYLIST: u32 = 1 << 1;

#[repr(C)]
pub struct JNINativeMethod {
    pub name: *const c_char,
    pub signature: *const c_char,
    pub fn_ptr: *mut c_void,
}

#[repr(C)]
pub struct AppSpecializeArgs {
    pub uid: *mut jint,
    pub gid: *mut jint,
    pub gids: *mut jintArray,
    pub runtime_flags: *mut jint,
    pub rlimits: *mut jobjectArray,
    pub mount_external: *mut jint,
    pub se_info: *mut jstring,
    pub nice_name: *mut jstring,
    pub instruction_set: *mut jstring,
    pub app_data_dir: *mut jstring,
    pub fds_to_ignore: *const jintArray,
    pub is_child_zygote: *const jboolean,
    pub is_top_app: *const jboolean,
    pub pkg_data_info_list: *const jobjectArray,
    pub whitelisted_data_info_list: *const jobjectArray,
    pub mount_data_dirs: *const jboolean,
    pub mount_storage_dirs: *const jboolean,
}

#[repr(C)]
pub struct ServerSpecializeArgs {
    pub uid: *mut jint,
    pub gid: *mut jint,
    pub gids: *mut jintArray,
    pub runtime_flags: *mut jint,
    pub permitted_capabilities: *mut jlong,
    pub effective_capabilities: *mut jlong,
}

#[repr(C)]
pub struct ModuleAbi {
    pub api_version: c_long,
    pub impl_: *mut c_void,
    pub pre_app_specialize: unsafe extern "C" fn(*mut c_void, *mut AppSpecializeArgs),
    pub post_app_specialize: unsafe extern "C" fn(*mut c_void, *const AppSpecializeArgs),
    pub pre_server_specialize: unsafe extern "C" fn(*mut c_void, *mut ServerSpecializeArgs),
    pub post_server_specialize: unsafe extern "C" fn(*mut c_void, *const ServerSpecializeArgs),
}

#[repr(C)]
pub struct ApiTable {
    pub impl_: *mut c_void,
    pub register_module: Option<unsafe extern "C" fn(*mut ApiTable, *mut ModuleAbi) -> bool>,
    pub hook_jni_native_methods:
        Option<unsafe extern "C" fn(*mut JNIEnv, *const c_char, *mut JNINativeMethod, c_int)>,
    pub plt_hook_register: Option<
        unsafe extern "C" fn(
            libc::dev_t,
            libc::ino_t,
            *const c_char,
            *mut c_void,
            *mut *mut c_void,
        ),
    >,
    pub exempt_fd: Option<unsafe extern "C" fn(c_int) -> bool>,
    pub plt_hook_commit: Option<unsafe extern "C" fn() -> bool>,
    pub connect_companion: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    pub set_option: Option<unsafe extern "C" fn(*mut c_void, ZygiskOption)>,
    pub get_module_dir: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    pub get_flags: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
}

#[derive(Clone, Copy)]
pub struct ZygiskApi {
    tbl: *mut ApiTable,
}

unsafe impl Send for ZygiskApi {}
unsafe impl Sync for ZygiskApi {}

impl ZygiskApi {
    pub unsafe fn from_raw(tbl: *mut ApiTable) -> Option<Self> {
        if tbl.is_null() {
            None
        } else {
            Some(Self { tbl })
        }
    }

    pub fn connect_companion(&self) -> c_int {
        unsafe {
            let tbl = &*self.tbl;
            match tbl.connect_companion {
                Some(f) => f(tbl.impl_),
                None => -1,
            }
        }
    }

    pub fn get_module_dir(&self) -> c_int {
        unsafe {
            let tbl = &*self.tbl;
            match tbl.get_module_dir {
                Some(f) => f(tbl.impl_),
                None => -1,
            }
        }
    }

    pub fn set_option(&self, opt: ZygiskOption) {
        unsafe {
            let tbl = &*self.tbl;
            if let Some(f) = tbl.set_option {
                f(tbl.impl_, opt);
            }
        }
    }

    pub fn get_flags(&self) -> u32 {
        unsafe {
            let tbl = &*self.tbl;
            match tbl.get_flags {
                Some(f) => f(tbl.impl_),
                None => 0,
            }
        }
    }

    pub fn exempt_fd(&self, fd: c_int) -> bool {
        unsafe {
            let tbl = &*self.tbl;
            match tbl.exempt_fd {
                Some(f) => f(fd),
                None => false,
            }
        }
    }

    pub unsafe fn plt_hook_register(
        &self,
        dev: libc::dev_t,
        inode: libc::ino_t,
        symbol: &str,
        new_func: *mut c_void,
        old_func: *mut *mut c_void,
    ) {
        let Ok(c_sym) = CString::new(symbol) else {
            return;
        };
        let tbl = &*self.tbl;
        if let Some(f) = tbl.plt_hook_register {
            f(dev, inode, c_sym.as_ptr(), new_func, old_func);
        }
    }

    pub fn plt_hook_commit(&self) -> bool {
        unsafe {
            let tbl = &*self.tbl;
            match tbl.plt_hook_commit {
                Some(f) => f(),
                None => false,
            }
        }
    }
}

impl CompanionConnector for ZygiskApi {
    fn connect_companion(&self) -> libc::c_int {
        ZygiskApi::connect_companion(self)
    }

    fn exempt_fd(&self, fd: libc::c_int) -> bool {
        ZygiskApi::exempt_fd(self, fd)
    }
}
