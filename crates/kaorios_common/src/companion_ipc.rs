// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project
//
// Bounded, partial-I/O-safe Zygisk Companion IPC Session & Framing Helpers.

pub const MAX_COMPANION_FRAME_BYTES: usize = 65536 + 256;

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
    if client_socket < 0 {
        return false;
    }

    let extract_line = |buffer: &mut Vec<u8>, out_cmd: &mut String| -> bool {
        let Some(nl) = buffer.iter().position(|&b| b == b'\n') else {
            return false;
        };
        let mut line = String::from_utf8_lossy(&buffer[..nl]).into_owned();
        if line.ends_with('\r') {
            line.pop();
        }
        *out_cmd = line;
        buffer.drain(..=nl);
        true
    };

    if extract_line(buffer, out_cmd) {
        return !out_cmd.is_empty();
    }

    loop {
        if buffer.len() > MAX_COMPANION_FRAME_BYTES {
            buffer.clear();
            return false;
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
            return false;
        }
        if ready == 0 {
            let buf_str = std::str::from_utf8(buffer).unwrap_or("");
            if !buffer.is_empty() && is_syntactically_complete_command(buf_str) {
                let mut line = buf_str.to_string();
                if line.ends_with('\r') {
                    line.pop();
                }
                *out_cmd = line;
                buffer.clear();
                return !out_cmd.is_empty();
            }
            return false;
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
            return false;
        }
        if n == 0 {
            if buffer.is_empty() {
                return false;
            }
            if extract_line(buffer, out_cmd) {
                return !out_cmd.is_empty();
            }
            let mut line = String::from_utf8_lossy(buffer).into_owned();
            if line.ends_with('\r') {
                line.pop();
            }
            *out_cmd = line;
            buffer.clear();
            return !out_cmd.is_empty();
        }

        buffer.extend_from_slice(&chunk[..n as usize]);
        if buffer.len() > MAX_COMPANION_FRAME_BYTES {
            buffer.clear();
            return false;
        }
        if extract_line(buffer, out_cmd) {
            return !out_cmd.is_empty();
        }
    }
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

    pub fn close_session(&mut self) {
        if self.retained_fd >= 0 {
            unsafe { libc::close(self.retained_fd) };
            self.retained_fd = -1;
        }
    }
}
