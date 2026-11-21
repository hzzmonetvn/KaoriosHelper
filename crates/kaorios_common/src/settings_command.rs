// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use std::collections::BTreeMap;
use std::ffi::{CStr, CString};
use std::time::{Duration, Instant};

const MAX_VALUE_BYTES: usize = 16 * 1024 * 1024;
const MAX_SNAPSHOT_BYTES: usize = 32 * 1024 * 1024;
const MAX_CONCURRENT_READS: usize = 6;

fn resolve_settings_binary() -> String {
    #[cfg(feature = "test-faults")]
    if let Ok(fixture) = std::env::var("KAORIOS_TEST_SETTINGS_BIN") {
        return fixture;
    }
    "/system/bin/settings".to_string()
}

struct SettingsChild {
    key: String,
    pid: libc::pid_t,
    fd: libc::c_int,
    eof: bool,
    exited: bool,
    status: libc::c_int,
    value: Vec<u8>,
}

impl SettingsChild {
    fn kill_and_reap(&mut self) {
        if self.fd >= 0 {
            unsafe { libc::close(self.fd) };
            self.fd = -1;
        }
        if !self.exited && self.pid > 0 {
            unsafe {
                libc::kill(-self.pid, libc::SIGKILL);
                libc::kill(self.pid, libc::SIGKILL);
                while libc::waitpid(self.pid, &mut self.status, 0) < 0 {
                    let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                    if err != libc::EINTR {
                        break;
                    }
                }
            }
            self.exited = true;
        }
    }
}

impl Drop for SettingsChild {
    fn drop(&mut self) {
        self.kill_and_reap();
    }
}

fn spawn_settings_get(c_bin: &CStr, key: &str) -> Result<SettingsChild, String> {
    let c_get = CString::new("get").unwrap();
    let c_global = CString::new("global").unwrap();
    let c_key = CString::new(key).map_err(|_| "Settings command start failed".to_string())?;
    let c_dev_null = CString::new("/dev/null").unwrap();

    let mut output = [0 as libc::c_int; 2];
    if unsafe { libc::pipe2(output.as_mut_ptr(), libc::O_CLOEXEC) } != 0 {
        return Err("Settings transport creation failed".to_string());
    }

    let pid = unsafe { libc::fork() };
    if pid == 0 {
        unsafe {
            libc::setpgid(0, 0);
            libc::close(output[0]);
            if libc::dup2(output[1], libc::STDOUT_FILENO) < 0 {
                libc::_exit(126);
            }
            libc::close(output[1]);
            let sink = libc::open(c_dev_null.as_ptr(), libc::O_RDWR | libc::O_CLOEXEC);
            if sink < 0
                || libc::dup2(sink, libc::STDERR_FILENO) < 0
                || libc::dup2(sink, libc::STDIN_FILENO) < 0
            {
                libc::_exit(126);
            }
            if sink > libc::STDERR_FILENO {
                libc::close(sink);
            }
            let argv = [
                c_bin.as_ptr(),
                c_get.as_ptr(),
                c_global.as_ptr(),
                c_key.as_ptr(),
                std::ptr::null(),
            ];
            libc::execv(c_bin.as_ptr(), argv.as_ptr());
            libc::_exit(127);
        }
    }
    unsafe { libc::close(output[1]) };
    if pid < 0 {
        unsafe { libc::close(output[0]) };
        return Err("Settings command start failed".to_string());
    }
    unsafe { libc::setpgid(pid, pid) };

    let flags = unsafe { libc::fcntl(output[0], libc::F_GETFL) };
    if flags < 0 || unsafe { libc::fcntl(output[0], libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0 {
        let mut child = SettingsChild {
            key: key.to_string(),
            pid,
            fd: output[0],
            eof: false,
            exited: false,
            status: 0,
            value: Vec::new(),
        };
        child.kill_and_reap();
        return Err("Settings command failed or exceeded bounds".to_string());
    }

    Ok(SettingsChild {
        key: key.to_string(),
        pid,
        fd: output[0],
        eof: false,
        exited: false,
        status: 0,
        value: Vec::new(),
    })
}

fn drain_child_stdout(child: &mut SettingsChild, total_bytes: &mut usize) -> Result<(), ()> {
    let mut buf = [0u8; 8192];
    loop {
        let n = unsafe { libc::read(child.fd, buf.as_mut_ptr().cast(), buf.len()) };
        if n == 0 {
            child.eof = true;
            return Ok(());
        }
        if n < 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err == libc::EINTR {
                continue;
            }
            if err != libc::EAGAIN && err != libc::EWOULDBLOCK {
                return Err(());
            }
            return Ok(());
        }
        let chunk = n as usize;
        child.value.extend_from_slice(&buf[..chunk]);
        *total_bytes += chunk;
        if child.value.len() > MAX_VALUE_BYTES || *total_bytes > MAX_SNAPSHOT_BYTES {
            return Err(());
        }
    }
}

pub fn read_global_setting(key: &str, deadline: Instant) -> Result<String, String> {
    let keys = [key.to_string()];
    let mut map = read_global_settings_pass(&keys, deadline)?;
    map.remove(key)
        .ok_or_else(|| "Settings command failed".to_string())
}

fn read_global_settings_pass(
    keys: &[String],
    deadline: Instant,
) -> Result<BTreeMap<String, String>, String> {
    let mut results = BTreeMap::new();
    if keys.is_empty() {
        return Ok(results);
    }

    let binary = resolve_settings_binary();
    let c_bin = CString::new(binary.as_str())
        .map_err(|_| "Settings transport creation failed".to_string())?;

    let mut next_idx = 0usize;
    let mut active: Vec<SettingsChild> = Vec::with_capacity(MAX_CONCURRENT_READS);
    let mut total_bytes = 0usize;

    while next_idx < keys.len() || !active.is_empty() {
        while active.len() < MAX_CONCURRENT_READS && next_idx < keys.len() {
            if Instant::now() >= deadline {
                return Err("Settings command failed or exceeded bounds".to_string());
            }
            let child = spawn_settings_get(&c_bin, &keys[next_idx])?;
            active.push(child);
            next_idx += 1;
        }

        let now = Instant::now();
        if now >= deadline {
            return Err("Settings command failed or exceeded bounds".to_string());
        }
        let remaining_ms = deadline.duration_since(now).as_millis().min(50) as libc::c_int;

        let mut pfds = Vec::with_capacity(active.len());
        let mut pfd_indices = Vec::with_capacity(active.len());
        for (idx, child) in active.iter().enumerate() {
            if !child.eof && child.fd >= 0 {
                pfds.push(libc::pollfd {
                    fd: child.fd,
                    events: libc::POLLIN | libc::POLLHUP,
                    revents: 0,
                });
                pfd_indices.push(idx);
            }
        }

        if !pfds.is_empty() {
            let nfds =
                libc::nfds_t::try_from(pfds.len()).unwrap_or(libc::nfds_t::MAX);
            let ready = unsafe { libc::poll(pfds.as_mut_ptr(), nfds, remaining_ms.max(1)) };
            if ready < 0 {
                let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if err != libc::EINTR {
                    return Err("Settings command failed or exceeded bounds".to_string());
                }
            } else if ready > 0 {
                for (p_idx, pfd) in pfds.iter().enumerate() {
                    if pfd.revents != 0 {
                        let a_idx = pfd_indices[p_idx];
                        if drain_child_stdout(&mut active[a_idx], &mut total_bytes).is_err()
                            || Instant::now() >= deadline
                        {
                            if total_bytes > MAX_SNAPSHOT_BYTES {
                                return Err("Settings snapshot too large".to_string());
                            }
                            return Err("Settings command failed or exceeded bounds".to_string());
                        }
                    }
                }
            }
        } else {
            std::thread::sleep(Duration::from_millis(2));
        }

        for child in &mut active {
            if !child.exited {
                let res = unsafe { libc::waitpid(child.pid, &mut child.status, libc::WNOHANG) };
                if res == child.pid {
                    child.exited = true;
                } else if res < 0 {
                    let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                    if err != libc::EINTR {
                        return Err("Settings command failed or exceeded bounds".to_string());
                    }
                }
            }
        }

        let mut i = 0;
        while i < active.len() {
            if active[i].eof && active[i].exited {
                let mut done = active.swap_remove(i);
                if done.fd >= 0 {
                    unsafe { libc::close(done.fd) };
                    done.fd = -1;
                }
                if !libc::WIFEXITED(done.status) || libc::WEXITSTATUS(done.status) != 0 {
                    return Err("Settings command failed".to_string());
                }
                let raw = std::mem::take(&mut done.value);
                let mut text =
                    String::from_utf8(raw).map_err(|_| "Settings command failed".to_string())?;
                if text.ends_with('\n') {
                    text.pop();
                }
                let k = std::mem::take(&mut done.key);
                results.insert(k, text);
            } else {
                i += 1;
            }
        }
    }

    Ok(results)
}

pub fn capture_global_settings(keys: &[String]) -> Result<BTreeMap<String, String>, String> {
    let deadline = Instant::now() + Duration::from_secs(30);
    let first = read_global_settings_pass(keys, deadline)?;
    let second = read_global_settings_pass(keys, deadline)?;
    if first != second {
        return Err("Settings changed during synchronization; retry explicitly".to_string());
    }
    Ok(first)
}
