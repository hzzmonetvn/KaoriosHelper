// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use std::collections::BTreeMap;
use std::ffi::CString;
use std::time::{Duration, Instant};

const MAX_VALUE_BYTES: usize = 16 * 1024 * 1024;
const MAX_SNAPSHOT_BYTES: usize = 32 * 1024 * 1024;

pub fn read_global_setting(key: &str, deadline: Instant) -> Result<String, String> {
    #[allow(unused_mut)]
    let mut binary = "/system/bin/settings".to_string();
    #[cfg(feature = "test-faults")]
    if let Ok(fixture) = std::env::var("KAORIOS_TEST_SETTINGS_BIN") {
        binary = fixture;
    }

    let c_bin = CString::new(binary.as_str())
        .map_err(|_| "Settings transport creation failed".to_string())?;
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
    let mut failed =
        flags < 0 || unsafe { libc::fcntl(output[0], libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0;
    let mut eof = false;
    let mut exited = false;
    let mut status: libc::c_int = 0;
    let mut value = Vec::new();

    while !failed && (!eof || !exited) {
        let now = Instant::now();
        if now >= deadline {
            failed = true;
            break;
        }
        let remaining_ms = deadline.duration_since(now).as_millis().min(50) as libc::c_int;
        let mut pfd = libc::pollfd {
            fd: if eof { -1 } else { output[0] },
            events: libc::POLLIN | libc::POLLHUP,
            revents: 0,
        };
        let ready = unsafe { libc::poll(&mut pfd, 1, remaining_ms.max(1)) };
        if ready < 0 {
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err != libc::EINTR {
                failed = true;
                break;
            }
        }
        if ready > 0 {
            let mut buf = [0u8; 8192];
            loop {
                let n = unsafe { libc::read(output[0], buf.as_mut_ptr().cast(), buf.len()) };
                if n == 0 {
                    eof = true;
                    break;
                }
                if n < 0 {
                    let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                    if err == libc::EINTR {
                        continue;
                    }
                    if err != libc::EAGAIN && err != libc::EWOULDBLOCK {
                        failed = true;
                    }
                    break;
                }
                value.extend_from_slice(&buf[..n as usize]);
                if value.len() > MAX_VALUE_BYTES || Instant::now() >= deadline {
                    failed = true;
                    break;
                }
            }
        }
        if !exited {
            let res = unsafe { libc::waitpid(pid, &mut status, libc::WNOHANG) };
            if res == pid {
                exited = true;
            } else if res < 0 {
                let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if err != libc::EINTR {
                    failed = true;
                }
            }
        }
    }

    unsafe { libc::close(output[0]) };
    if failed {
        unsafe {
            libc::kill(-pid, libc::SIGKILL);
            if !exited {
                libc::kill(pid, libc::SIGKILL);
                while libc::waitpid(pid, &mut status, 0) < 0 {
                    let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                    if err != libc::EINTR {
                        break;
                    }
                }
            }
        }
        return Err("Settings command failed or exceeded bounds".to_string());
    }

    if !libc::WIFEXITED(status) || libc::WEXITSTATUS(status) != 0 {
        return Err("Settings command failed".to_string());
    }
    let mut text = String::from_utf8(value).map_err(|_| "Settings command failed".to_string())?;
    if text.ends_with('\n') {
        text.pop();
    }
    Ok(text)
}

pub fn capture_global_settings(keys: &[String]) -> Result<BTreeMap<String, String>, String> {
    let deadline = Instant::now() + Duration::from_secs(30);
    let mut first = BTreeMap::new();
    let mut total = 0usize;
    for key in keys {
        let value = read_global_setting(key, deadline)?;
        total += value.len();
        if total > MAX_SNAPSHOT_BYTES {
            return Err("Settings snapshot too large".to_string());
        }
        first.insert(key.clone(), value);
    }
    for (key, val) in &first {
        if &read_global_setting(key, deadline)? != val {
            return Err("Settings changed during synchronization; retry explicitly".to_string());
        }
    }
    Ok(first)
}
