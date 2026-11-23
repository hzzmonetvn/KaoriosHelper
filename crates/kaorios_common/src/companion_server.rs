// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use crate::ack_protocol::{
    parse_runtime_ack, runtime_ack_boot_id, runtime_ack_has_live_identity_with_bootstrap,
    runtime_ack_read_process_identity, validate_runtime_ack,
};
use crate::companion_ipc::{
    read_next_companion_command_status, write_companion_bytes, CompanionEngineState,
    CompanionReadStatus, MAX_COMPANION_PAYLOAD_BYTES,
};
use crate::native_store::NativeStore;
use crate::runtime_snapshot::{
    read_runtime_config_snapshot, write_runtime_ack_for_current_config_with_bootstrap,
    RuntimeConfigSnapshot,
};
use crate::{log_debug, log_error, log_info, log_warn, DEFAULT_DATA_DIR};
use std::ffi::CString;
use std::path::Path;

pub fn is_trusted_cpu_spoof_source(path: &str) -> bool {
    if path.is_empty() || path.contains('\0') {
        return false;
    }
    let leaf = match path.rfind('/') {
        Some(pos) => &path[pos + 1..],
        None => path,
    };
    if leaf != "cpuinfo_spoof" {
        return false;
    }
    let Ok(c_path) = CString::new(path) else {
        return false;
    };
    let mut lst: libc::stat = unsafe { std::mem::zeroed() };
    if unsafe { libc::lstat(c_path.as_ptr(), &mut lst) } != 0 {
        return false;
    }
    let euid = unsafe { libc::geteuid() };
    if !crate::is_regular_file(lst.st_mode) || lst.st_nlink != 1 {
        return false;
    }
    if lst.st_uid != euid || (lst.st_mode & 0o022) != 0 {
        return false;
    }
    if lst.st_size <= 0 || lst.st_size > 1024 * 1024 {
        return false;
    }
    true
}

pub fn handle_companion_session(client_socket: libc::c_int) {
    handle_companion_session_with_dir(client_socket, DEFAULT_DATA_DIR, 5000);
}

pub fn handle_companion_session_with_dir(
    client_socket: libc::c_int,
    data_dir: &str,
    idle_timeout_ms: libc::c_int,
) {
    log_debug!("KaoriosHelper root companion socket connected: {client_socket}");

    let mut cr: libc::ucred = unsafe { std::mem::zeroed() };
    let mut len = std::mem::size_of::<libc::ucred>() as libc::socklen_t;
    let mut peer_pid: libc::pid_t = -1;
    let mut have_peercred = false;
    if unsafe {
        libc::getsockopt(
            client_socket,
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            (&mut cr as *mut libc::ucred).cast(),
            &mut len,
        )
    } == 0
    {
        peer_pid = cr.pid;
        have_peercred = true;
        log_debug!(
            "KaoriosHelper companion: peer pid={} uid={} gid={}",
            cr.pid,
            cr.uid,
            cr.gid
        );
    } else {
        log_warn!(
            "KaoriosHelper companion: getsockopt(SO_PEERCRED) failed: {}",
            std::io::Error::last_os_error()
        );
    }

    let resolve_live_uid = |pid: libc::pid_t| -> Option<libc::uid_t> {
        if let Some((uid, _, _)) = runtime_ack_read_process_identity(pid) {
            return Some(uid);
        }
        if have_peercred {
            return Some(cr.uid);
        }
        None
    };

    let run_in_target_ns = |target_pid: libc::pid_t, f: &dyn Fn() -> i32| -> i32 {
        if !have_peercred || target_pid <= 1 || target_pid != peer_pid {
            log_warn!(
                "KaoriosHelper companion: invalid or mismatched target_pid={target_pid} (peer={peer_pid})"
            );
            return -1;
        }
        let Some(current_uid) = resolve_live_uid(target_pid) else {
            log_warn!(
                "KaoriosHelper companion: caller pid={target_pid} uid unavailable; refusing namespace operations"
            );
            return -1;
        };
        if current_uid == 0 {
            log_warn!(
                "KaoriosHelper companion: caller pid={target_pid} uid={current_uid} is unspecialized; refusing namespace operations"
            );
            return -1;
        }
        if unsafe { libc::kill(target_pid, 0) } != 0 {
            log_warn!(
                "KaoriosHelper companion: target pid={target_pid} is no longer alive: {}",
                std::io::Error::last_os_error()
            );
            return -1;
        }

        let ns_path = CString::new(format!("/proc/{target_pid}/ns/mnt")).unwrap();
        let target_ns = unsafe { libc::open(ns_path.as_ptr(), libc::O_RDONLY | libc::O_CLOEXEC) };
        if target_ns < 0 {
            log_warn!(
                "KaoriosHelper companion: failed to open target mnt_ns: {}",
                std::io::Error::last_os_error()
            );
            return -1;
        }

        let mut target_st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(target_ns, &mut target_st) } != 0 {
            unsafe { libc::close(target_ns) };
            return -1;
        }

        let root_ns_path = CString::new("/proc/1/ns/mnt").unwrap();
        let root_ns =
            unsafe { libc::open(root_ns_path.as_ptr(), libc::O_RDONLY | libc::O_CLOEXEC) };
        if root_ns >= 0 {
            let mut root_st: libc::stat = unsafe { std::mem::zeroed() };
            let same = unsafe { libc::fstat(root_ns, &mut root_st) } == 0
                && target_st.st_ino == root_st.st_ino;
            unsafe { libc::close(root_ns) };
            if same {
                log_error!(
                    "KaoriosHelper companion: Refusing mount operation: target pid={} shares mount namespace with PID 1/root (inode={})",
                    target_pid,
                    target_st.st_ino
                );
                unsafe { libc::close(target_ns) };
                return -1;
            }
        }

        let self_ns_path = CString::new("/proc/self/ns/mnt").unwrap();
        let self_ns =
            unsafe { libc::open(self_ns_path.as_ptr(), libc::O_RDONLY | libc::O_CLOEXEC) };
        if self_ns >= 0 {
            let mut self_st: libc::stat = unsafe { std::mem::zeroed() };
            let same = unsafe { libc::fstat(self_ns, &mut self_st) } == 0
                && target_st.st_ino == self_st.st_ino;
            unsafe { libc::close(self_ns) };
            if same {
                log_error!(
                    "KaoriosHelper companion: Refusing mount operation: target pid={} shares mount namespace with companion daemon (inode={})",
                    target_pid,
                    target_st.st_ino
                );
                unsafe { libc::close(target_ns) };
                return -1;
            }
        }

        let child = unsafe { libc::fork() };
        if child < 0 {
            log_warn!(
                "KaoriosHelper companion: fork for mount namespace failed: {}",
                std::io::Error::last_os_error()
            );
            unsafe { libc::close(target_ns) };
            return -1;
        }
        if child == 0 {
            unsafe {
                if libc::setns(target_ns, libc::CLONE_NEWNS) != 0 {
                    libc::close(target_ns);
                    libc::_exit(1);
                }
                libc::close(target_ns);
                let proc_c = CString::new("/proc").unwrap();
                libc::mount(
                    std::ptr::null(),
                    proc_c.as_ptr(),
                    std::ptr::null(),
                    libc::MS_SLAVE | libc::MS_REC,
                    std::ptr::null(),
                );
                let rc = f();
                libc::_exit(if rc == 0 { 0 } else { 1 });
            }
        }
        unsafe { libc::close(target_ns) };
        let mut status: libc::c_int = 0;
        loop {
            let r = unsafe { libc::waitpid(child, &mut status, 0) };
            if r >= 0 {
                break;
            }
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err != libc::EINTR {
                return -1;
            }
        }
        if libc::WIFEXITED(status) && libc::WEXITSTATUS(status) == 0 {
            0
        } else {
            -1
        }
    };

    let mut read_buffer = Vec::new();
    let mut keepalive_server = false;
    loop {
        let command = match read_next_companion_command_status(
            client_socket,
            &mut read_buffer,
            idle_timeout_ms,
        ) {
            CompanionReadStatus::Command(cmd) => cmd,
            CompanionReadStatus::Timeout => {
                if keepalive_server && peer_pid > 1 && unsafe { libc::kill(peer_pid, 0) } == 0 {
                    continue;
                }
                break;
            }
            CompanionReadStatus::Closed => break,
        };

        if let Some(engine) = command.strip_prefix("fetch_state:") {
            let state_bytes: Option<Vec<u8>> =
                if matches!(engine, "hma" | "pif" | "copg" | "tee" | "kaorios") {
                    let boot_id = runtime_ack_boot_id();
                    let (enabled, snapshot) = if engine == "kaorios" {
                        let snap =
                            read_runtime_config_snapshot(data_dir, "kaorios", "kaorios.json")
                                .unwrap_or_else(|_| {
                                    let bytes = NativeStore::open(data_dir)
                                        .and_then(|s| s.read("kaorios.json", true))
                                        .unwrap_or_default();
                                    RuntimeConfigSnapshot {
                                        bytes,
                                        boot_id: boot_id.clone(),
                                        ..Default::default()
                                    }
                                });
                        (!snap.bytes.is_empty(), snap)
                    } else {
                        let enabled = match engine {
                            "hma" | "pif" => {
                                !Path::new(&format!("{data_dir}/{engine}.disabled")).exists()
                            }
                            "copg" | "tee" => {
                                Path::new(&format!("{data_dir}/{engine}.enabled")).exists()
                            }
                            _ => false,
                        };
                        let snapshot = if !enabled {
                            RuntimeConfigSnapshot::default()
                        } else {
                            match engine {
                                "hma" => read_runtime_config_snapshot(data_dir, "hma", "hma.json")
                                    .unwrap_or_default(),
                                "pif" => read_runtime_config_snapshot(data_dir, "pif", "pif.json")
                                    .unwrap_or_default(),
                                "copg" => {
                                    read_runtime_config_snapshot(data_dir, "copg", "spoof.json")
                                        .unwrap_or_default()
                                }
                                "tee" => {
                                    let bytes = NativeStore::open(data_dir)
                                        .and_then(|s| s.read("keybox.xml", true))
                                        .unwrap_or_default();
                                    RuntimeConfigSnapshot {
                                        bytes,
                                        boot_id: boot_id.clone(),
                                        ..Default::default()
                                    }
                                }
                                _ => RuntimeConfigSnapshot::default(),
                            }
                        };
                        (enabled, snapshot)
                    };
                    let tee_runtime_active =
                        Path::new(&format!("{data_dir}/tee.runtime_active")).exists();
                    if engine == "hma"
                        && enabled
                        && have_peercred
                        && (cr.uid == 0 || resolve_live_uid(peer_pid) == Some(1000))
                    {
                        keepalive_server = true;
                    }
                    let state = CompanionEngineState {
                        enabled,
                        tee_runtime_active,
                        boot_id,
                        snapshot,
                    };
                    serde_json::to_vec(&state)
                        .ok()
                        .filter(|b| b.len() <= MAX_COMPANION_PAYLOAD_BYTES)
                } else {
                    None
                };

            match state_bytes {
                Some(payload) => {
                    let len_u32 = u32::try_from(payload.len()).unwrap_or(0);
                    let ok_status = 0i32.to_ne_bytes();
                    let len_bytes = len_u32.to_ne_bytes();
                    if !write_companion_bytes(client_socket, &ok_status)
                        || !write_companion_bytes(client_socket, &len_bytes)
                        || (!payload.is_empty() && !write_companion_bytes(client_socket, &payload))
                    {
                        break;
                    }
                }
                None => {
                    let err_status = (-1i32).to_ne_bytes();
                    if !write_companion_bytes(client_socket, &err_status) {
                        break;
                    }
                }
            }
            continue;
        }

        let mut result: i32 = -1;

        if command == "mount_spoof" || command.starts_with("mount_cpu:") {
            let mut target_pid = peer_pid;
            let mut spoof_path = format!("{data_dir}/cpuinfo_spoof");

            if let Some(rem) = command.strip_prefix("mount_cpu:") {
                if let Some((pid_str, custom_src)) = rem.split_once(':') {
                    let req_pid: libc::pid_t = pid_str.parse().unwrap_or(-1);
                    if req_pid != peer_pid {
                        log_warn!(
                            "KaoriosHelper companion: rejected mount_cpu PID mismatch (req={req_pid} peer={peer_pid})"
                        );
                        target_pid = -1;
                    }
                    if !custom_src.is_empty() {
                        spoof_path = custom_src.to_string();
                    }
                } else {
                    target_pid = -1;
                }
            }

            if target_pid > 1 {
                if !is_trusted_cpu_spoof_source(&spoof_path) {
                    let legacy = "/data/adb/modules/COPG/cpuinfo_spoof";
                    if spoof_path == format!("{data_dir}/cpuinfo_spoof")
                        && is_trusted_cpu_spoof_source(legacy)
                    {
                        spoof_path = legacy.to_string();
                    }
                }
                if is_trusted_cpu_spoof_source(&spoof_path) {
                    let c_src = CString::new(spoof_path.as_str()).unwrap();
                    let c_dst = CString::new("/proc/cpuinfo").unwrap();
                    result = run_in_target_ns(target_pid, &|| unsafe {
                        libc::umount2(c_dst.as_ptr(), libc::MNT_DETACH);
                        if libc::mount(
                            c_src.as_ptr(),
                            c_dst.as_ptr(),
                            std::ptr::null(),
                            libc::MS_BIND,
                            std::ptr::null(),
                        ) == 0
                        {
                            log_info!(
                                "KaoriosHelper companion: mounted {spoof_path} -> /proc/cpuinfo for pid={target_pid}"
                            );
                            0
                        } else {
                            log_warn!(
                                "KaoriosHelper companion: mount(MS_BIND) failed: {}",
                                std::io::Error::last_os_error()
                            );
                            -1
                        }
                    });
                } else {
                    log_warn!(
                        "KaoriosHelper companion: untrusted or missing cpuinfo_spoof at {spoof_path}"
                    );
                }
            }
        } else if command == "unmount_spoof" {
            let c_dst = CString::new("/proc/cpuinfo").unwrap();
            result = run_in_target_ns(peer_pid, &|| unsafe {
                let uerr = libc::umount2(c_dst.as_ptr(), libc::MNT_DETACH);
                if uerr == 0 {
                    log_info!(
                        "KaoriosHelper companion: unmounted /proc/cpuinfo for pid={peer_pid}"
                    );
                    0
                } else {
                    log_debug!(
                        "KaoriosHelper companion: umount2 failed (or wasn't mounted): {}",
                        std::io::Error::last_os_error()
                    );
                    uerr
                }
            });
        } else if let Some(payload) = command.strip_prefix("ack:") {
            if have_peercred {
                let allow_bootstrap = cr.uid == 0;
                if let Some(ack) = parse_runtime_ack(payload) {
                    let uid_matches = resolve_live_uid(peer_pid)
                        .is_some_and(|live| ack.uid == live && (cr.uid == 0 || cr.uid == live));
                    if ack.pid != peer_pid
                        || !uid_matches
                        || validate_runtime_ack(&ack).is_err()
                        || !runtime_ack_has_live_identity_with_bootstrap(&ack, allow_bootstrap)
                    {
                        log_warn!(
                            "KaoriosHelper companion: rejected unauthorized runtime ack from pid={} peer_uid={} ({})",
                            peer_pid,
                            cr.uid,
                            ack.process_name
                        );
                        result = -1;
                    } else {
                        result = if write_runtime_ack_for_current_config_with_bootstrap(
                            data_dir,
                            &ack,
                            allow_bootstrap,
                        ) {
                            0
                        } else {
                            -2
                        };
                    }
                }
            }
        } else {
            log_warn!("KaoriosHelper companion: unrecognized command '{command}'");
        }

        if !write_companion_bytes(client_socket, &result.to_ne_bytes()) {
            break;
        }
    }

    unsafe { libc::close(client_socket) };
}
