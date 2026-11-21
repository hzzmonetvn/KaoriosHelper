// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use crate::engine::Engine;
use crate::jni_sys::{jint, JNIEnv};
use crate::zygisk_abi::{AppSpecializeArgs, ZygiskApi};
use kaorios_common::companion_ipc::{CompanionConnector, CompanionSession};
use kaorios_common::{log_debug, log_info, log_warn, DEFAULT_DATA_DIR};
use std::fs;
use std::path::Path;
use std::sync::Mutex;

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct KeyboxInfo {
    pub valid: bool,
    pub has_ecdsa: bool,
    pub has_rsa: bool,
    pub cert_count: usize,
    pub device_id: String,
}

pub struct TeeEngine {
    data_dir: String,
    api: Option<ZygiskApi>,
    env: *mut JNIEnv,
    prefetched_keybox: Option<String>,
    keybox_mutex: Mutex<()>,
}

unsafe impl Send for TeeEngine {}

impl Default for TeeEngine {
    fn default() -> Self {
        Self::new(DEFAULT_DATA_DIR.to_string())
    }
}

impl TeeEngine {
    pub fn new(data_dir: String) -> Self {
        Self {
            data_dir,
            api: None,
            env: std::ptr::null_mut(),
            prefetched_keybox: None,
            keybox_mutex: Mutex::new(()),
        }
    }

    pub fn parse_keybox(&self) -> KeyboxInfo {
        let _guard = self.keybox_mutex.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(ref xml) = self.prefetched_keybox {
            return Self::parse_keybox_content(xml);
        }
        if self.api.is_some() {
            return KeyboxInfo::default();
        }
        let keybox_path = format!("{}/keybox.xml", self.data_dir);
        let Ok(meta) = fs::metadata(&keybox_path) else {
            return KeyboxInfo::default();
        };
        if meta.len() == 0 {
            return KeyboxInfo::default();
        }
        let Ok(mut bytes) = fs::read(&keybox_path) else {
            return KeyboxInfo::default();
        };
        let content = String::from_utf8_lossy(&bytes).into_owned();
        let info = Self::parse_keybox_content(&content);
        bytes.fill(0);
        info
    }

    pub fn parse_keybox_content(content: &str) -> KeyboxInfo {
        let mut info = KeyboxInfo::default();
        if content.trim().is_empty() {
            return info;
        }

        let mut tag_stack: Vec<String> = Vec::new();
        let mut has_keybox_root = false;
        let mut in_device_id = false;
        let mut device_id_buffer = String::new();
        let mut xml_error = false;
        let mut i = 0usize;
        let raw = content.as_bytes();

        while i < raw.len() {
            if raw[i] == b'<' {
                if i + 1 < raw.len() && raw[i + 1] == b'?' {
                    let Some(close_rel) = content[i + 2..].find("?>") else {
                        xml_error = true;
                        break;
                    };
                    i = i + 2 + close_rel + 2;
                    continue;
                }
                if content[i..].starts_with("<!--") {
                    let Some(close_rel) = content[i + 4..].find("-->") else {
                        xml_error = true;
                        break;
                    };
                    i = i + 4 + close_rel + 3;
                    continue;
                }
                let Some(end_rel) = content[i..].find('>') else {
                    xml_error = true;
                    break;
                };
                let mut tag_body = content[i + 1..i + end_rel].to_string();
                i = i + end_rel + 1;
                if tag_body.is_empty() {
                    xml_error = true;
                    break;
                }

                let is_closing = tag_body.starts_with('/');
                let is_self_closing = tag_body.ends_with('/');
                if is_closing {
                    let closing_name = tag_body[1..]
                        .split([' ', '\t', '\r', '\n'])
                        .next()
                        .unwrap_or("")
                        .to_string();
                    if tag_stack.last().map(String::as_str) != Some(closing_name.as_str()) {
                        log_warn!("TEE: Unbalanced closing XML tag </{closing_name}> in keybox.xml");
                        xml_error = true;
                        break;
                    }
                    if closing_name == "DeviceId" {
                        in_device_id = false;
                        info.device_id = device_id_buffer.clone();
                    }
                    tag_stack.pop();
                } else {
                    if is_self_closing {
                        tag_body.pop();
                    }
                    let mut parts = tag_body.split_whitespace();
                    let tag_name = parts.next().unwrap_or("").to_string();

                    if matches!(
                        tag_name.as_str(),
                        "AndroidAttestation" | "KeyboxContent" | "Keybox"
                    ) && (tag_stack.is_empty() || tag_name == "Keybox")
                    {
                        has_keybox_root = true;
                    }

                    if tag_name == "Keybox" && info.device_id.is_empty() {
                        let lower_body = tag_body.to_ascii_lowercase();
                        if let Some(did_pos) = lower_body.find("deviceid=") {
                            let val_start = did_pos + 9;
                            if let Some(quote) = tag_body.as_bytes().get(val_start).copied() {
                                if quote == b'"' || quote == b'\'' {
                                    if let Some(val_end_rel) =
                                        tag_body[val_start + 1..].find(quote as char)
                                    {
                                        info.device_id = tag_body
                                            [val_start + 1..val_start + 1 + val_end_rel]
                                            .to_string();
                                    }
                                }
                            }
                        }
                    }

                    if tag_name == "Key" {
                        for attr in parts {
                            if let Some(rest) = attr.strip_prefix("algorithm=") {
                                let algo = rest
                                    .trim_matches(|c| c == '"' || c == '\'')
                                    .to_ascii_lowercase();
                                if algo == "ecdsa" || algo == "ec" {
                                    info.has_ecdsa = true;
                                }
                                if algo == "rsa" {
                                    info.has_rsa = true;
                                }
                            }
                        }
                    } else if tag_name == "Certificate" {
                        info.cert_count += 1;
                    } else if tag_name == "DeviceId" {
                        in_device_id = true;
                        device_id_buffer.clear();
                    }

                    if !is_self_closing {
                        tag_stack.push(tag_name);
                    }
                }
            } else {
                if in_device_id && !matches!(raw[i], b'\r' | b'\n' | b'\t' | b' ') {
                    device_id_buffer.push(raw[i] as char);
                }
                i += 1;
            }
        }

        if !xml_error
            && tag_stack.is_empty()
            && has_keybox_root
            && (info.has_ecdsa || info.has_rsa)
            && info.cert_count > 0
        {
            info.valid = true;
        }

        info
    }

    pub fn has_valid_keybox(&self) -> bool {
        self.parse_keybox().valid
    }

    pub fn is_attestation_simulation_active(&self) -> bool {
        false
    }
}

impl Engine for TeeEngine {
    fn name(&self) -> &'static str {
        "tee"
    }

    fn is_enabled(&self) -> bool {
        Path::new(&format!("{}/tee.enabled", self.data_dir)).exists()
    }

    fn on_load(&mut self, api: Option<ZygiskApi>, env: *mut JNIEnv) {
        self.api = api;
        self.env = env;
        log_debug!("TEE Engine onLoad");
    }

    fn should_handle_app(&mut self, process_name: &str, _uid: jint) -> bool {
        if !matches!(
            process_name,
            "com.google.android.gms.unstable" | "com.google.android.gms"
        ) {
            return false;
        }
        if let Some(ref api) = self.api {
            let mut cs = CompanionSession::new();
            let Some(state) = cs.fetch_engine_state(Some(api as &dyn CompanionConnector), "tee")
            else {
                return false;
            };
            if !state.enabled {
                self.prefetched_keybox = None;
                return false;
            }
            self.prefetched_keybox = Some(state.snapshot.bytes);
            return true;
        }
        self.is_enabled()
    }

    fn pre_app_specialize(&mut self, _args: *mut AppSpecializeArgs, process_name: &str) {
        log_info!("TEE preAppSpecialize for process: {process_name}");
    }

    fn post_app_specialize(&mut self, _args: *const AppSpecializeArgs, process_name: &str) {
        let info = self.parse_keybox();
        if !info.valid {
            log_warn!(
                "TEE: Keybox XML missing or invalid in {}. Hardware attestation simulation inactive.",
                self.data_dir
            );
            return;
        }
        log_info!(
            "TEE: Keybox XML structurally validated for {} (ECDSA={}, RSA={}, certs={}). Note: Keystore2 system daemon interception is not bundled; runtime status is INCOMPLETE_RUNTIME.",
            process_name,
            info.has_ecdsa,
            info.has_rsa,
            info.cert_count
        );
    }
}
