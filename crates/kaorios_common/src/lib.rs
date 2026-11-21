// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

pub mod ack_protocol;
pub mod companion_ipc;
pub mod companion_server;
pub mod config_identity;
pub mod logging;
pub mod native_store;
pub mod runtime_snapshot;
pub mod screen_capability;
pub mod settings_adapter;
pub mod settings_command;

pub const DEFAULT_MOD_DIR: &str = "/data/adb/modules/kaorioshelper";
pub const DEFAULT_DATA_DIR: &str = "/data/adb/modules/kaorioshelper/data";

#[inline]
#[allow(clippy::unnecessary_cast)]
pub fn is_regular_file(mode: u32) -> bool {
    (mode & (libc::S_IFMT as u32)) == (libc::S_IFREG as u32)
}
