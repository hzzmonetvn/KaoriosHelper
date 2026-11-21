// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project
//
// Bounded, partial-I/O-safe Zygisk Companion IPC Session & Framing Helpers.

use crate::runtime_snapshot::RuntimeConfigSnapshot;
use serde::{Deserialize, Serialize};

pub const MAX_COMPANION_FRAME_BYTES: usize = 65536 + 256;
pub const MAX_COMPANION_PAYLOAD_BYTES: usize = 16 * 1024 * 1024;

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CompanionEngineState {
    pub enabled: bool,
    pub tee_runtime_active: bool,
    pub boot_id: String,
    pub snapshot: RuntimeConfigSnapshot,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CompanionReadStatus {
    Command(String),
    Timeout,
    Closed,
}

pub trait CompanionConnector {
    fn connect_companion(&self) -> libc::c_int;
    fn exempt_fd(&self, fd: libc::c_int) -> bool;
}

pub fn write_companion_bytes(fd: libc::c_int, data: &[u8]) -> bool {
    if fd < 0 {
        return false;
    }
    let mut offset = 0usize;
    while offset < data.len() {
        let n = unsafe { libc::write(fd, data[offset..].as_ptr().cast(), data.len() - offset) };
        if n < 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err == libc::EINTR {
                continue;
            }
            return false;
        }
        if n == 0 {
            return false;
        }
        offset += n as usize;
    }
    true
}

pub fn read_companion_bytes(fd: libc::c_int, data: &mut [u8]) -> bool {
    if fd < 0 {
        return false;
    }
    let mut offset = 0usize;
    while offset < data.len() {
        let n = unsafe { libc::read(fd, data[offset..].as_mut_ptr().cast(), data.len() - offset) };
        if n < 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err == libc::EINTR {
                continue;
            }
            return false;
        }
        if n == 0 {
            return false;
        }
        offset += n as usize;
    }
    true
}

pub fn is_syntactically_complete_command(buf: &str) -> bool {
    if buf == "mount_spoof" || buf == "unmount_spoof" {
        return true;
    }
    if let Some(engine) = buf.strip_prefix("fetch_state:") {
        return matches!(engine, "hma" | "pif" | "copg" | "tee");
    }
    if let Some(rest) = buf.strip_prefix("mount_cpu:") {
        if rest.contains(':') {
            return true;
        }
    }
    if buf.starts_with("ack:") && buf.len() > 5 && buf.as_bytes()[4] == b'{' {
        let mut depth = 0i32;
        let mut in_string = false;
        let mut escaped = false;
        let bytes = buf.as_bytes();
        for (i, &c) in bytes.iter().enumerate().skip(4) {
            if escaped {
                escaped = false;
                continue;
            }
            if c == b'\\' && in_string {
                escaped = true;
                continue;
            }
            if c == b'"' {
                in_string = !in_string;
                continue;
            }
            if in_string {
                continue;
            }
            if c == b'{' {
                depth += 1;
            } else if c == b'}' {
                depth -= 1;
                if depth == 0 && i + 1 == bytes.len() {
                    return true;
                }
                if depth < 0 {
                    return false;
                }
            }
        }
    }
    false
}

pub fn read_next_companion_command(
    client_socket: libc::c_int,
    buffer: &mut Vec<u8>,
    out_cmd: &mut String,
    idle_timeout_ms: libc::c_int,
) -> bool {
    out_cmd.clear();
    match read_next_companion_command_status(client_socket, buffer, idle_timeout_ms) {
        CompanionReadStatus::Command(cmd) => {
            *out_cmd = cmd;
            true
        }
        CompanionReadStatus::Timeout | CompanionReadStatus::Closed => false,
    }
}

pub fn read_next_companion_command_status(
    client_socket: libc::c_int,
    buffer: &mut Vec<u8>,
    idle_timeout_ms: libc::c_int,
) -> CompanionReadStatus {
    if client_socket < 0 {
        return CompanionReadStatus::Closed;
    }

    let extract_line = |buffer: &mut Vec<u8>| -> Option<String> {
        let nl = buffer.iter().position(|&b| b == b'\n')?;
        let mut line = String::from_utf8_lossy(&buffer[..nl]).into_owned();
        if line.ends_with('\r') {
            line.pop();
        }
        buffer.drain(..=nl);
        Some(line)
    };

    if let Some(line) = extract_line(buffer) {
        return if line.is_empty() {
            CompanionReadStatus::Closed
        } else {
            CompanionReadStatus::Command(line)
        };
    }

    loop {
        if buffer.len() > MAX_COMPANION_FRAME_BYTES {
            buffer.clear();
            return CompanionReadStatus::Closed;
        }
        let buf_str = std::str::from_utf8(buffer).unwrap_or("");
        let wait_ms = if !buffer.is_empty() && is_syntactically_complete_command(buf_str) {
            10
        } else {
            idle_timeout_ms
        };
        let mut pfd = libc::pollfd {
            fd: client_socket,
            events: libc::POLLIN,
            revents: 0,
        };
        let ready = loop {
            let r = unsafe { libc::poll(&mut pfd, 1, wait_ms) };
            if r < 0 {
                let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if err == libc::EINTR {
                    continue;
                }
            }
            break r;
        };

        if ready < 0 {
            return CompanionReadStatus::Closed;
        }
        if ready == 0 {
            let buf_str = std::str::from_utf8(buffer).unwrap_or("");
            if !buffer.is_empty() && is_syntactically_complete_command(buf_str) {
                let mut line = buf_str.to_string();
                if line.ends_with('\r') {
                    line.pop();
                }
                buffer.clear();
                return if line.is_empty() {
                    CompanionReadStatus::Closed
                } else {
                    CompanionReadStatus::Command(line)
                };
            }
            return if buffer.is_empty() {
                CompanionReadStatus::Timeout
            } else {
                CompanionReadStatus::Closed
            };
        }

        let mut chunk = [0u8; 8192];
        let n = loop {
            let r = unsafe { libc::read(client_socket, chunk.as_mut_ptr().cast(), chunk.len()) };
            if r < 0 {
                let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if err == libc::EINTR {
                    continue;
                }
            }
            break r;
        };

        if n < 0 {
            return CompanionReadStatus::Closed;
        }
        if n == 0 {
            if buffer.is_empty() {
                return CompanionReadStatus::Closed;
            }
            if let Some(line) = extract_line(buffer) {
                return if line.is_empty() {
                    CompanionReadStatus::Closed
                } else {
                    CompanionReadStatus::Command(line)
                };
            }
            let mut line = String::from_utf8_lossy(buffer).into_owned();
            if line.ends_with('\r') {
                line.pop();
            }
            buffer.clear();
            return if line.is_empty() {
                CompanionReadStatus::Closed
            } else {
                CompanionReadStatus::Command(line)
            };
        }

        buffer.extend_from_slice(&chunk[..n as usize]);
        if buffer.len() > MAX_COMPANION_FRAME_BYTES {
            buffer.clear();
            return CompanionReadStatus::Closed;
        }
        if let Some(line) = extract_line(buffer) {
            return if line.is_empty() {
                CompanionReadStatus::Closed
            } else {
                CompanionReadStatus::Command(line)
            };
        }
    }
}

fn read_framed_payload(fd: libc::c_int) -> Result<Option<Vec<u8>>, ()> {
    let mut status_bytes = [0u8; std::mem::size_of::<i32>()];
    if !read_companion_bytes(fd, &mut status_bytes) {
        return Err(());
    }
    if i32::from_ne_bytes(status_bytes) != 0 {
        return Ok(None);
    }
    let mut len_bytes = [0u8; std::mem::size_of::<u32>()];
    if !read_companion_bytes(fd, &mut len_bytes) {
        return Err(());
    }
    let len = u32::from_ne_bytes(len_bytes) as usize;
    if len > MAX_COMPANION_PAYLOAD_BYTES {
        return Err(());
    }
    let mut payload = vec![0u8; len];
    if len > 0 && !read_companion_bytes(fd, &mut payload) {
        return Err(());
    }
    Ok(Some(payload))
}

pub struct CompanionSession {
    retained_fd: libc::c_int,
}

impl Default for CompanionSession {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for CompanionSession {
    fn drop(&mut self) {
        self.close_session();
    }
}

impl CompanionSession {
    pub const fn new() -> Self {
        Self { retained_fd: -1 }
    }

    pub fn pre_specialize(&mut self, api: Option<&dyn CompanionConnector>) {
        self.close_session();
        let Some(api) = api else {
            return;
        };
        let fd = api.connect_companion();
        if fd < 0 {
            return;
        }
        if api.exempt_fd(fd) {
            self.retained_fd = fd;
        } else {
            unsafe { libc::close(fd) };
        }
    }

    pub fn has_retained_socket(&self) -> bool {
        self.retained_fd >= 0
    }

    pub fn request(
        &mut self,
        api: Option<&dyn CompanionConnector>,
        cmd: &str,
        out_resp: &mut i32,
    ) -> bool {
        *out_resp = -1;
        if cmd.is_empty()
            || cmd.len() > MAX_COMPANION_FRAME_BYTES - 2
            || cmd.contains('\n')
            || cmd.contains('\0')
        {
            return false;
        }

        let mut resp_bytes = [0u8; std::mem::size_of::<i32>()];
        if self.retained_fd >= 0 {
            let framed = format!("{cmd}\n");
            if write_companion_bytes(self.retained_fd, framed.as_bytes())
                && read_companion_bytes(self.retained_fd, &mut resp_bytes)
            {
                *out_resp = i32::from_ne_bytes(resp_bytes);
                return true;
            }
            self.close_session();
        }

        let Some(api) = api else {
            return false;
        };
        let sock = api.connect_companion();
        if sock < 0 {
            return false;
        }

        let mut ok = write_companion_bytes(sock, cmd.as_bytes());
        if ok {
            unsafe { libc::shutdown(sock, libc::SHUT_WR) };
            ok = read_companion_bytes(sock, &mut resp_bytes);
            if ok {
                *out_resp = i32::from_ne_bytes(resp_bytes);
            }
        }
        unsafe { libc::close(sock) };
        ok
    }

    pub fn request_payload(
        &mut self,
        api: Option<&dyn CompanionConnector>,
        cmd: &str,
    ) -> Option<Vec<u8>> {
        if cmd.is_empty()
            || cmd.len() > MAX_COMPANION_FRAME_BYTES - 2
            || cmd.contains('\n')
            || cmd.contains('\0')
        {
            return None;
        }

        if self.retained_fd >= 0 {
            let framed = format!("{cmd}\n");
            if write_companion_bytes(self.retained_fd, framed.as_bytes()) {
                if let Ok(opt) = read_framed_payload(self.retained_fd) {
                    return opt;
                }
            }
            self.close_session();
        }

        let api = api?;
        let sock = api.connect_companion();
        if sock < 0 {
            return None;
        }

        let mut payload = None;
        if write_companion_bytes(sock, cmd.as_bytes()) {
            unsafe { libc::shutdown(sock, libc::SHUT_WR) };
            if let Ok(opt) = read_framed_payload(sock) {
                payload = opt;
            }
        }
        unsafe { libc::close(sock) };
        payload
    }

    pub fn fetch_engine_state(
        &mut self,
        api: Option<&dyn CompanionConnector>,
        engine: &str,
    ) -> Option<CompanionEngineState> {
        let cmd = format!("fetch_state:{engine}");
        let bytes = self.request_payload(api, &cmd)?;
        serde_json::from_slice(&bytes).ok()
    }

    pub fn close_session(&mut self) {
        if self.retained_fd >= 0 {
            unsafe { libc::close(self.retained_fd) };
            self.retained_fd = -1;
        }
    }
}
