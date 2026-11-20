// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project
// KaoriosHelper Native Control CLI (helperctl)

use kaorios_common::ack_protocol::{
    read_runtime_ack, runtime_ack_boot_id, runtime_ack_matches_current, runtime_ack_now_millis,
    AckEventType,
};
use kaorios_common::native_store::{validate_native_json, Changes, NativeStore};
use kaorios_common::settings_adapter::{
    adapt_settings, parse_pif_settings, settings_keys, validate_keybox_xml_structure,
};
use kaorios_common::settings_command::capture_global_settings;
use kaorios_common::{DEFAULT_DATA_DIR, DEFAULT_MOD_DIR};
use serde::Serialize;
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::ffi::CString;
use std::fs;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct EngineStatus {
    #[serde(skip_serializing)]
    name: String,
    installed: bool,
    enabled: bool,
    config_valid: bool,
    runtime_available: bool,
    process_loaded: bool,
    hook_registered: bool,
    hook_initialized: String,
    runtime_observed: bool,
    device_verified: bool,
    stored_generation: u64,
    requested_generation: u64,
    applied_generation: u64,
    last_error: String,
    last_runtime_event: String,
    last_runtime_timestamp: u64,
    last_verified_timestamp: u64,
    pending_reload: bool,
    status: String,
    details: String,
}

impl EngineStatus {
    fn new(name: &str) -> Self {
        Self {
            name: name.to_string(),
            installed: true,
            enabled: false,
            config_valid: false,
            runtime_available: false,
            process_loaded: false,
            hook_registered: false,
            hook_initialized: "NOT_INITIALIZED".to_string(),
            runtime_observed: false,
            device_verified: false,
            stored_generation: 0,
            requested_generation: 0,
            applied_generation: 0,
            last_error: String::new(),
            last_runtime_event: "NONE".to_string(),
            last_runtime_timestamp: 0,
            last_verified_timestamp: 0,
            pending_reload: false,
            status: "DISABLED".to_string(),
            details: String::new(),
        }
    }
}

struct HelperCtl {
    mod_dir: String,
    data_dir: String,
}

impl HelperCtl {
    fn new() -> Self {
        let mod_dir = if let Ok(env_mod) = std::env::var("KAORIOS_MOD_DIR") {
            if !env_mod.is_empty() {
                env_mod
            } else {
                Self::detect_mod_dir()
            }
        } else {
            Self::detect_mod_dir()
        };

        let data_dir = match std::env::var("KAORIOS_DATA_DIR") {
            Ok(d) if !d.is_empty() => d,
            _ => format!("{mod_dir}/data"),
        };

        Self { mod_dir, data_dir }
    }

    fn detect_mod_dir() -> String {
        if let Ok(exe) = std::env::current_exe() {
            if let Some(parent) = exe.parent() {
                if parent.join("module.prop").exists() {
                    return parent.to_string_lossy().into_owned();
                }
            }
        }
        DEFAULT_MOD_DIR.to_string()
    }

    fn get_version(&self) -> String {
        let prop_file = PathBuf::from(&self.mod_dir).join("module.prop");
        if let Ok(text) = fs::read_to_string(prop_file) {
            for line in text.lines() {
                if let Some(v) = line.strip_prefix("version=") {
                    return v.to_string();
                }
            }
        }
        "v1.0.0".to_string()
    }

    fn is_module_disabled(&self) -> bool {
        Path::new(&format!("{}/disable", self.mod_dir)).exists()
    }

    fn config_name(engine: &str) -> Result<String, String> {
        match engine {
            "pif" | "hma" | "kaorios" => Ok(format!("{engine}.json")),
            "copg" => Ok("spoof.json".to_string()),
            "tee" => Ok("keybox.xml".to_string()),
            _ => Err("Invalid config engine".to_string()),
        }
    }

    fn valid_config(engine: &str, content: &str) -> bool {
        if engine == "tee" {
            validate_keybox_xml_structure(content)
        } else {
            validate_native_json(engine, content)
        }
    }

    fn read_generation(text: &str) -> Option<u64> {
        if text.is_empty() {
            return Some(0);
        }
        let trimmed_end = text.trim_end_matches([' ', '\r', '\n', '\t']);
        if trimmed_end.is_empty() || !trimmed_end.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        trimmed_end.parse::<u64>().ok()
    }

    fn parse_generation(text: &str) -> Result<u64, String> {
        Self::read_generation(text)
            .ok_or_else(|| "Invalid configuration generation; explicit migration required".to_string())
    }

    fn valid_request(request: &Value) -> bool {
        let Some(obj) = request.as_object() else {
            return false;
        };
        let Some(token) = obj.get("requestId").and_then(|v| v.as_str()) else {
            return false;
        };
        let Some(boot) = obj.get("bootId").and_then(|v| v.as_str()) else {
            return false;
        };
        let Some(not_before) = obj.get("notBeforeMillis").and_then(|v| v.as_u64()) else {
            return false;
        };
        if token.len() != 32
            || !token
                .bytes()
                .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
        {
            return false;
        }
        boot == runtime_ack_boot_id() && not_before > 0 && not_before <= runtime_ack_now_millis()
    }

    fn stage_request(changes: &mut Changes, engine: &str, generation: u64) -> Result<(), String> {
        let mut entropy = [0u8; 16];
        let mut offset = 0usize;
        while offset < entropy.len() {
            let n = unsafe {
                libc::getrandom(
                    entropy[offset..].as_mut_ptr().cast(),
                    entropy.len() - offset,
                    0,
                )
            };
            if n < 0 {
                let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if err == libc::EINTR {
                    continue;
                }
                return Err("Runtime request identity unavailable".to_string());
            }
            if n == 0 {
                return Err("Runtime request identity unavailable".to_string());
            }
            offset += n as usize;
        }
        let mut token = String::with_capacity(32);
        for b in entropy {
            use std::fmt::Write;
            let _ = write!(token, "{b:02x}");
        }
        let boot = runtime_ack_boot_id();
        let now = runtime_ack_now_millis();
        if boot.is_empty() || now == 0 {
            return Err("Runtime boot identity unavailable".to_string());
        }
        changes.insert(
            format!("{engine}.request.json"),
            Some(
                json!({
                    "requestId": token,
                    "bootId": boot,
                    "notBeforeMillis": now,
                })
                .to_string(),
            ),
        );
        changes.insert(
            format!("{engine}.requested_gen"),
            Some(format!("{generation}\n")),
        );
        changes.insert(format!("{engine}.reload_pending"), Some(String::new()));
        changes.insert(format!("{engine}.verified"), None);
        Ok(())
    }

    fn stage_config(
        store: &NativeStore,
        changes: &mut Changes,
        engine: &str,
        content: &str,
        rollback: bool,
    ) -> Result<u64, String> {
        let filename = Self::config_name(engine)?;
        for leaf in [
            filename.clone(),
            format!("{filename}.lkg"),
            format!("{engine}.stored_gen"),
            format!("{engine}.requested_gen"),
            format!("{engine}.request.json"),
            format!("{engine}.reload_pending"),
            format!("{engine}.verified"),
        ] {
            store.check(&leaf)?;
        }
        if !Self::valid_config(engine, content) {
            return Err("Configuration validation failed".to_string());
        }
        let previous = store.read(&filename, true)?;
        let mut gen = Self::parse_generation(&store.read(&format!("{engine}.stored_gen"), true)?)?;
        if gen == u64::MAX {
            return Err("Generation exhausted".to_string());
        }
        gen += 1;
        if !rollback && Self::valid_config(engine, &previous) {
            changes.insert(format!("{filename}.lkg"), Some(previous));
        }
        changes.insert(filename, Some(content.to_string()));
        changes.insert(format!("{engine}.stored_gen"), Some(format!("{gen}\n")));
        Self::stage_request(changes, engine, gen)?;
        Ok(gen)
    }

    fn read_config_source(path: &str) -> Result<String, String> {
        let c_path = CString::new(path).map_err(|_| "Source open failed".to_string())?;
        let fd = unsafe {
            libc::open(
                c_path.as_ptr(),
                libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC,
            )
        };
        if fd < 0 {
            return Err("Source open failed".to_string());
        }
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        let euid = unsafe { libc::geteuid() };
        if unsafe { libc::fstat(fd, &mut st) } != 0
            || (st.st_mode & libc::S_IFMT) != libc::S_IFREG
            || st.st_uid != euid
            || (st.st_mode & 0o022) != 0
            || st.st_size > 16 * 1024 * 1024
        {
            unsafe { libc::close(fd) };
            return Err("Untrusted source file".to_string());
        }
        let mut out = Vec::new();
        let mut buf = [0u8; 8192];
        loop {
            let n = unsafe { libc::read(fd, buf.as_mut_ptr().cast(), buf.len()) };
            if n == 0 {
                break;
            }
            if n < 0 {
                let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if err == libc::EINTR {
                    continue;
                }
                unsafe { libc::close(fd) };
                return Err("Source read failed".to_string());
            }
            out.extend_from_slice(&buf[..n as usize]);
            if out.len() > 16 * 1024 * 1024 {
                unsafe { libc::close(fd) };
                return Err("Source too large".to_string());
            }
        }
        unsafe { libc::close(fd) };
        String::from_utf8(out).map_err(|_| "Source read failed".to_string())
    }

    fn get_engine_statuses(&self) -> Result<BTreeMap<String, EngineStatus>, String> {
        let store = NativeStore::open(&self.data_dir)?;
        for engine in ["hma", "pif", "tee", "copg", "kaorios"] {
            let filename = Self::config_name(engine)?;
            for name in [
                filename,
                format!("{engine}.stored_gen"),
                format!("{engine}.requested_gen"),
                format!("{engine}.request.json"),
                format!("{engine}.enabled"),
                format!("{engine}.disabled"),
                format!("{engine}.reload_pending"),
                format!("{engine}.verified"),
            ] {
                store.check(&name)?;
            }
        }
        store.check("COPG.json")?;
        store.check("pif.prop")?;

        let dex_exists = fs::metadata(format!("{}/classes.dex", self.data_dir))
            .is_ok_and(|m| m.len() > 0)
            || fs::metadata(format!("{}/classes.dex", self.mod_dir)).is_ok_and(|m| m.len() > 0);

        let mut res = BTreeMap::new();
        for name in ["hma", "pif", "tee", "copg", "kaorios"] {
            let mut s = EngineStatus::new(name);
            s.enabled = name != "kaorios"
                && if name == "hma" || name == "pif" {
                    !store.exists(&format!("{name}.disabled"))?
                } else {
                    store.exists(&format!("{name}.enabled"))?
                };
            s.runtime_available =
                name == "pif" || name == "copg" || (name == "hma" && dex_exists);
            let stored = Self::read_generation(&store.read(&format!("{name}.stored_gen"), true)?);
            let requested =
                Self::read_generation(&store.read(&format!("{name}.requested_gen"), true)?);
            s.stored_generation = stored.unwrap_or(0);
            s.requested_generation = requested.unwrap_or(0);
            let content = store.read(&Self::config_name(name)?, true)?;
            s.config_valid = Self::valid_config(name, &content);
            s.pending_reload = store.exists(&format!("{name}.reload_pending"))?;

            let ack = read_runtime_ack(&format!("{}/runtime", self.data_dir), name);
            let req_str = store.read(&format!("{name}.request.json"), true)?;
            let request: Value = serde_json::from_str(&req_str).unwrap_or(Value::Null);
            let token = request
                .as_object()
                .and_then(|o| o.get("requestId"))
                .and_then(|v| v.as_str())
                .unwrap_or("");
            let not_before = request
                .as_object()
                .and_then(|o| o.get("notBeforeMillis"))
                .and_then(|v| v.as_u64())
                .unwrap_or(0);

            let current = stored.is_some()
                && requested.is_some()
                && s.stored_generation > 0
                && s.enabled
                && s.config_valid
                && s.runtime_available
                && ack.is_some()
                && s.stored_generation == s.requested_generation
                && Self::valid_request(&request)
                && ack.as_ref().is_some_and(|a| {
                    a.timestamp >= not_before
                        && runtime_ack_matches_current(
                            a,
                            name,
                            s.stored_generation,
                            content.as_bytes(),
                            token,
                        )
                });

            let initialized = current
                && ack.as_ref().is_some_and(|a| {
                    a.init_result == 0
                        && a.error_code == 0
                        && matches!(
                            a.event(),
                            Some(AckEventType::HookInitialized) | Some(AckEventType::HookObserved)
                        )
                });
            s.process_loaded = current
                && ack.as_ref().is_some_and(|a| {
                    a.event() != Some(AckEventType::ApplyFailed)
                        && a.init_result == 0
                        && a.error_code == 0
                });
            s.hook_registered = s.process_loaded
                && (ack
                    .as_ref()
                    .is_some_and(|a| a.event() == Some(AckEventType::HookRegistered))
                    || initialized);
            s.runtime_observed = initialized
                && ack
                    .as_ref()
                    .is_some_and(|a| a.event() == Some(AckEventType::HookObserved));
            s.applied_generation = if initialized {
                ack.as_ref().map(|a| a.config_generation).unwrap_or(0)
            } else {
                0
            };
            s.last_runtime_event = ack
                .as_ref()
                .map(|a| a.event_type.clone())
                .unwrap_or_else(|| "NONE".to_string());
            s.last_runtime_timestamp = ack.as_ref().map(|a| a.timestamp).unwrap_or(0);

            if stored.is_none() || requested.is_none() {
                s.status = "INVALID_CONFIG".to_string();
                s.hook_initialized = "NOT_INITIALIZED".to_string();
                s.pending_reload = true;
                s.last_error = "Invalid configuration generation metadata".to_string();
                if stored.is_none() || stored == Some(0) {
                    let mut mig_content = content.clone();
                    if mig_content.is_empty() && name == "copg" {
                        mig_content = store.read("COPG.json", true)?;
                    }
                    let mut mig_valid = Self::valid_config(name, &mig_content);
                    if !mig_valid && mig_content.is_empty() && name == "pif" {
                        let legacy_prop = store.read("pif.prop", true)?;
                        if !legacy_prop.is_empty() {
                            if let Ok(parsed) = parse_pif_settings(&legacy_prop) {
                                if let Ok(pretty) = serde_json::to_string_pretty(&Value::Object(parsed)) {
                                    mig_content = format!("{pretty}\n");
                                    mig_valid = Self::valid_config(name, &mig_content);
                                }
                            }
                        }
                    }
                    s.details = if mig_valid {
                        format!("Run config migrate {name} to repair metadata explicitly")
                    } else {
                        format!(
                            "Restore valid {} from backup, then run config migrate {name}",
                            Self::config_name(name)?
                        )
                    };
                } else if name == "kaorios" {
                    s.details = "Republish the validated stored payload with config set kaorios FILE to renew request metadata".to_string();
                } else {
                    s.details = format!(
                        "Renew request metadata with reload {name}; prepare-boot also renews valid managed configurations"
                    );
                }
                res.insert(name.to_string(), s);
                continue;
            }

            if name == "kaorios" {
                s.status = if s.config_valid {
                    "INCOMPLETE_RUNTIME"
                } else {
                    "NOT_CONFIGURED"
                }
                .to_string();
                s.hook_initialized = "INCOMPLETE_RUNTIME".to_string();
                s.details =
                    "Configuration storage available; Kaorios runtime backend not implemented"
                        .to_string();
                res.insert(name.to_string(), s);
                continue;
            }

            if !s.enabled {
                s.status = "DISABLED".to_string();
                s.hook_initialized = "DISABLED".to_string();
                s.details =
                    "Desired enablement is off; existing processes may still require restart"
                        .to_string();
                res.insert(name.to_string(), s);
                continue;
            }

            if !s.config_valid {
                s.status = "NOT_CONFIGURED".to_string();
                s.hook_initialized = "NOT_INITIALIZED".to_string();
                s.details =
                    "Canonical configuration missing or invalid; legacy payloads require explicit migration"
                        .to_string();
                res.insert(name.to_string(), s);
                continue;
            }

            if !s.runtime_available {
                s.status = "INCOMPLETE_RUNTIME".to_string();
                s.hook_initialized = "INCOMPLETE_RUNTIME".to_string();
                s.details = if name == "hma" {
                    "HMA classes.dex payload missing"
                } else {
                    "TEE runtime backend not implemented"
                }
                .to_string();
                res.insert(name.to_string(), s);
                continue;
            }

            s.hook_initialized = if initialized {
                "INITIALIZED"
            } else {
                "NOT_INITIALIZED"
            }
            .to_string();
            s.pending_reload = !initialized;

            if current
                && ack.as_ref().is_some_and(|a| {
                    a.event() == Some(AckEventType::ApplyFailed)
                        || a.init_result != 0
                        || a.error_code != 0
                })
            {
                s.status = "FAILED".to_string();
                s.hook_initialized = "FAILED".to_string();
                s.last_error = "Runtime reported configuration application failure".to_string();
                s.details =
                    "Current process reported failure; retry or restart remains pending"
                        .to_string();
            } else if initialized {
                s.status = if s.runtime_observed {
                    "DEVICE_UNVERIFIED"
                } else {
                    "READY"
                }
                .to_string();
                s.last_error.clear();
                s.details = if s.runtime_observed {
                    "Current process observed; physical-device verification pending"
                } else {
                    "Current process initialized; runtime observation pending"
                }
                .to_string();
            } else {
                s.status = "PENDING_RELOAD".to_string();
                s.last_error.clear();
                s.details = if s.stored_generation == 0 {
                    "Legacy configuration requires explicit migration"
                } else {
                    "Awaiting current boot/process/configuration/request acknowledgement"
                }
                .to_string();
            }

            res.insert(name.to_string(), s);
        }
        Ok(res)
    }

    fn update_status_json(&self) -> bool {
        if fs::create_dir_all(&self.data_dir).is_err() {
            return false;
        }
        let Ok(engines) = self.get_engine_statuses() else {
            return false;
        };
        let ver = self.get_version();
        let mod_dis = self.is_module_disabled();
        let j = json!({
            "schemaVersion": 1,
            "moduleVersion": ver,
            "moduleState": if mod_dis { "DISABLED" } else { "ACTIVE" },
            "engines": engines,
        });
        let Ok(serialized) = serde_json::to_string_pretty(&j) else {
            return false;
        };
        let tmp_path = format!("{}/status.json.tmp.{}", self.data_dir, std::process::id());
        let dest_path = format!("{}/status.json", self.data_dir);
        if fs::write(&tmp_path, format!("{serialized}\n")).is_err() {
            return false;
        }
        let c_tmp = CString::new(tmp_path.as_str()).unwrap();
        let c_dst = CString::new(dest_path.as_str()).unwrap();
        unsafe {
            libc::chmod(c_tmp.as_ptr(), 0o600);
            libc::rename(c_tmp.as_ptr(), c_dst.as_ptr()) == 0
        }
    }

    fn cmd_status(&self, json_mode: bool) -> Result<i32, String> {
        let engines = self.get_engine_statuses()?;
        let ver = self.get_version();
        let mod_dis = self.is_module_disabled();
        if json_mode {
            let j = json!({
                "schemaVersion": 1,
                "module": {
                    "id": "kaorioshelper",
                    "version": ver,
                    "active": !mod_dis,
                },
                "engines": engines,
            });
            println!(
                "{}",
                serde_json::to_string_pretty(&j).map_err(|e| e.to_string())?
            );
            return Ok(0);
        }

        println!("===============================================");
        println!("         KaoriosHelper Runtime Status          ");
        println!("===============================================");
        println!("Module Version: {ver}");
        println!(
            "Module State:   {}\n",
            if mod_dis { "DISABLED" } else { "ACTIVE" }
        );
        println!("Subsystem Engines (Verified Runtime Assessment):");
        for (name, eng) in &engines {
            println!("  • {name} Engine:");
            println!(
                "      Installed:        {}",
                if eng.installed { "YES" } else { "NO" }
            );
            println!(
                "      Enabled:          {}",
                if eng.enabled { "YES" } else { "NO" }
            );
            println!(
                "      Config Valid:     {}",
                if eng.config_valid { "YES" } else { "NO" }
            );
            println!(
                "      Runtime Avail:    {}",
                if eng.runtime_available { "YES" } else { "NO" }
            );
            println!(
                "      Process Loaded:   {}",
                if eng.process_loaded { "YES" } else { "NO" }
            );
            println!(
                "      Hook Registered:  {}",
                if eng.hook_registered { "YES" } else { "NO" }
            );
            println!("      Hook Status:      {}", eng.hook_initialized);
            println!(
                "      Runtime Observed: {}",
                if eng.runtime_observed { "YES" } else { "NO" }
            );
            println!(
                "      Device Verified:  {}",
                if eng.device_verified { "YES" } else { "NO" }
            );
            println!("      Stored Gen:       {}", eng.stored_generation);
            println!("      Requested Gen:    {}", eng.requested_generation);
            println!("      Applied Gen:      {}", eng.applied_generation);
            println!(
                "      Pending Reload:   {}",
                if eng.pending_reload { "YES" } else { "NO" }
            );
            println!("      Last Event:       {}", eng.last_runtime_event);
            println!("      Status:           {}", eng.status);
            if !eng.last_error.is_empty() {
                println!("      Last Error:       {}", eng.last_error);
            }
            println!("      Details:          {}", eng.details);
        }
        println!("\nStorage Directory: {}", self.data_dir);
        println!("===============================================");
        Ok(0)
    }

    fn cmd_verify(&self, engine: &str, clear_mode: bool) -> i32 {
        let targets: Vec<&str> = if engine == "all" || engine.is_empty() {
            vec!["hma", "pif", "copg", "tee"]
        } else if matches!(engine, "hma" | "pif" | "copg" | "tee") {
            vec![engine]
        } else {
            eprintln!("Unknown engine for verification: {engine} (valid: hma, pif, copg, tee, all)");
            return 1;
        };

        if clear_mode {
            match NativeStore::open(&self.data_dir) {
                Ok(store) => {
                    for eng in &targets {
                        if let Err(e) = store.remove(&format!("{eng}.verified")) {
                            eprintln!("{e}");
                            return 1;
                        }
                        println!("External verification cleared for engine '{eng}'.");
                    }
                }
                Err(e) => {
                    eprintln!("{e}");
                    return 1;
                }
            }
            self.update_status_json();
            return 0;
        }

        for eng in &targets {
            println!("[NOT_VERIFIED] {eng}: no external device probe provider configured. Local readiness is not device proof.");
        }
        1
    }

    fn cmd_version(&self, json_mode: bool) -> i32 {
        let ver = self.get_version();
        if json_mode {
            let result = json!({
                "moduleId": "kaorioshelper",
                "version": ver,
                "protocolVersion": 1,
                "statusSchemaVersion": 1,
                "capabilities": {
                    "configSet": true,
                    "configValidate": true,
                    "configRollback": true,
                    "settingsGlobalSync": true,
                    "configMigration": true,
                    "recoverableConfigTransactions": true,
                    "runtimeAckV2": true,
                    "redactedConfigRead": true,
                    "atomicMultiFileSnapshot": false,
                    "structuredMutationErrors": false,
                    "deviceVerification": false,
                    "screenCapture": false
                }
            });
            println!("{result}");
        } else {
            println!("KaoriosHelper {ver}");
        }
        0
    }

    fn cmd_toggle(&self, engine: &str, enabled: bool) -> i32 {
        if !matches!(engine, "hma" | "pif" | "tee" | "copg") {
            eprintln!("Unknown runtime engine");
            return 1;
        }
        let res = (|| -> Result<(), String> {
            let store = NativeStore::open(&self.data_dir)?;
            let mut changes = Changes::new();
            let default_on = engine == "hma" || engine == "pif";
            let marker = format!(
                "{engine}{}",
                if default_on { ".disabled" } else { ".enabled" }
            );
            changes.insert(
                marker,
                if enabled != default_on {
                    Some(String::new())
                } else {
                    None
                },
            );
            let gen = Self::parse_generation(&store.read(&format!("{engine}.stored_gen"), true)?)?;
            Self::stage_request(&mut changes, engine, gen)?;
            store.transaction(&changes)
        })();
        match res {
            Ok(()) => {
                println!("PENDING: Desired enablement recorded; restart may be required.");
                0
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        }
    }

    fn cmd_prepare_boot(&self) -> i32 {
        let res = (|| -> Result<bool, String> {
            let store = NativeStore::open(&self.data_dir)?;
            let mut changes = Changes::new();
            let mut failed = false;
            for name in ["hma", "pif", "tee", "copg"] {
                let stored_text = store.read(&format!("{name}.stored_gen"), true)?;
                let Some(gen) = Self::read_generation(&stored_text) else {
                    eprintln!("{name}: invalid stored generation; explicit migration required");
                    failed = true;
                    continue;
                };
                if gen == 0 {
                    continue;
                }
                let content = store.read(&Self::config_name(name)?, true)?;
                if !Self::valid_config(name, &content) {
                    eprintln!("{name}: boot configuration validation failed");
                    failed = true;
                    continue;
                }
                let req_text = store.read(&format!("{name}.request.json"), true)?;
                let request: Value = serde_json::from_str(&req_text).unwrap_or(Value::Null);
                let req_gen = Self::read_generation(&store.read(&format!("{name}.requested_gen"), true)?);
                if req_gen == Some(gen) && Self::valid_request(&request) {
                    continue;
                }
                Self::stage_request(&mut changes, name, gen)?;
            }
            store.transaction(&changes)?;
            println!("Valid engine boot requests prepared; runtime acknowledgement pending");
            Ok(failed)
        })();
        match res {
            Ok(failed) => {
                if failed {
                    1
                } else {
                    0
                }
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        }
    }

    fn cmd_reload(&self, engine: &str) -> i32 {
        let targets: Vec<&str> = if engine == "all" || engine.is_empty() {
            vec!["hma", "pif", "tee", "copg"]
        } else if matches!(engine, "hma" | "pif" | "tee" | "copg") {
            vec![engine]
        } else {
            eprintln!("Unknown engine: {engine} (valid: hma, pif, tee, copg, all)");
            return 1;
        };

        let res = (|| -> Result<(), String> {
            let store = NativeStore::open(&self.data_dir)?;
            let mut changes = Changes::new();
            for eng in targets {
                let gen = Self::parse_generation(&store.read(&format!("{eng}.stored_gen"), true)?)?;
                Self::stage_request(&mut changes, eng, gen)?;
            }
            store.transaction(&changes)
        })();
        match res {
            Ok(()) => {
                println!("PENDING: Reload request recorded; existing processes may require restart.");
                0
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        }
    }

    fn publish_config(&self, engine: &str, src_path: &str, rollback: bool) -> i32 {
        let res = (|| -> Result<(String, u64), String> {
            let filename = Self::config_name(engine)?;
            let store = NativeStore::open(&self.data_dir)?;
            let content = if rollback {
                store.read(&format!("{filename}.lkg"), false)?
            } else {
                Self::read_config_source(src_path)?
            };
            let mut changes = Changes::new();
            let gen = Self::stage_config(&store, &mut changes, engine, &content, rollback)?;
            store.transaction(&changes)?;
            Ok((filename, gen))
        })();
        match res {
            Ok((filename, gen)) => {
                println!("Configuration published: {filename} (generation {gen}).");
                0
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        }
    }

    fn cmd_config_validate(&self, engine: &str, source: &str) -> i32 {
        let res = (|| -> Result<(), String> {
            let filename = Self::config_name(engine)?;
            let content = if source.is_empty() {
                let store = NativeStore::open(&self.data_dir)?;
                store.read(&filename, false)?
            } else {
                Self::read_config_source(source)?
            };
            if !Self::valid_config(engine, &content) {
                return Err("Configuration validation failed".to_string());
            }
            Ok(())
        })();
        match res {
            Ok(()) => {
                println!("{engine}: valid");
                0
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        }
    }

    fn cmd_config_sync(&self, engine: &str) -> i32 {
        let res = (|| -> Result<Vec<&'static str>, String> {
            let keys = settings_keys(engine)?;
            if keys.is_empty() {
                return Err("Invalid synchronization engine".to_string());
            }
            let settings = capture_global_settings(&keys)?;
            let store = NativeStore::open(&self.data_dir)?;
            let mut changes = Changes::new();
            let engines: Vec<&'static str> = if engine == "all" {
                vec!["pif", "tee", "copg", "hma", "kaorios"]
            } else {
                vec![match engine {
                    "pif" => "pif",
                    "tee" => "tee",
                    "copg" => "copg",
                    "hma" => "hma",
                    "kaorios" => "kaorios",
                    _ => return Err("Invalid synchronization engine".to_string()),
                }]
            };
            let mut changed = Vec::new();
            for name in engines {
                let existing = store.read(&Self::config_name(name)?, true)?;
                let Some(adapted) = adapt_settings(name, &existing, &settings)? else {
                    continue;
                };
                if name == "copg" && existing.is_empty() && store.exists("COPG.json")? {
                    return Err(
                        "Legacy COPG configuration requires explicit migration before synchronization"
                            .to_string(),
                    );
                }
                Self::stage_config(&store, &mut changes, name, &adapted, false)?;
                changed.push(name);
            }
            store.transaction(&changes)?;
            Ok(changed)
        })();
        match res {
            Ok(changed) => {
                if changed.is_empty() {
                    println!("No configuration changes");
                } else {
                    for name in changed {
                        println!("{name}: stored; runtime application pending");
                    }
                }
                0
            }
            Err(e) => {
                eprintln!(
                    "Synchronization failed: {e}; publication may require journal recovery"
                );
                1
            }
        }
    }

    fn cmd_config_migrate(&self, engine: &str) -> i32 {
        let res = (|| -> Result<(), String> {
            let engines: Vec<&str> = if engine == "all" {
                vec!["pif", "tee", "copg", "hma", "kaorios"]
            } else if matches!(engine, "pif" | "tee" | "copg" | "hma" | "kaorios") {
                vec![engine]
            } else {
                return Err("Invalid config engine".to_string());
            };
            let store = NativeStore::open(&self.data_dir)?;
            let mut changes = Changes::new();
            for name in engines {
                let filename = Self::config_name(name)?;
                let generation = Self::read_generation(&store.read(&format!("{name}.stored_gen"), true)?);
                let mut content = store.read(&filename, true)?;
                if content.is_empty() && name == "copg" {
                    content = store.read("COPG.json", true)?;
                }
                if content.is_empty() && name == "pif" {
                    let legacy_prop = store.read("pif.prop", true)?;
                    if !legacy_prop.is_empty() {
                        let parsed = parse_pif_settings(&legacy_prop)
                            .map_err(|_| "Legacy configuration validation failed".to_string())?;
                        let pretty = serde_json::to_string_pretty(&Value::Object(parsed))
                            .map_err(|_| "Legacy configuration validation failed".to_string())?;
                        content = format!("{pretty}\n");
                    }
                }
                if content.is_empty() {
                    if generation.is_none() {
                        return Err(format!("{name}: valid payload required before metadata repair"));
                    }
                    continue;
                }
                if !Self::valid_config(name, &content) {
                    return Err("Legacy configuration validation failed".to_string());
                }
                match generation {
                    None => {
                        changes.insert(filename, Some(content));
                        changes.insert(format!("{name}.stored_gen"), Some("1\n".to_string()));
                        Self::stage_request(&mut changes, name, 1)?;
                    }
                    Some(0) => {
                        Self::stage_config(&store, &mut changes, name, &content, false)?;
                    }
                    Some(_) => {}
                }
            }
            store.transaction(&changes)
        })();
        match res {
            Ok(()) => {
                println!("Migration recorded; existing schemas and payload fields preserved");
                0
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        }
    }

    fn redact_xml_tag(input: &str, tag_name: &str, canonical_tag: &str) -> String {
        let lower = input.to_ascii_lowercase();
        let open_prefix = format!("<{tag_name}");
        let close_tag = format!("</{tag_name}>");
        let mut result = String::with_capacity(input.len());
        let mut pos = 0usize;
        while pos < input.len() {
            let Some(rel_start) = lower[pos..].find(&open_prefix) else {
                result.push_str(&input[pos..]);
                break;
            };
            let start = pos + rel_start;
            let after_name = start + open_prefix.len();
            if let Some(next) = lower.as_bytes().get(after_name).copied() {
                if !matches!(next, b'>' | b' ' | b'\t' | b'\r' | b'\n' | b'/') {
                    result.push_str(&input[pos..after_name]);
                    pos = after_name;
                    continue;
                }
            }
            result.push_str(&input[pos..start]);
            let Some(rel_end) = lower[after_name..].find('>') else {
                result.push_str(&format!("<{canonical_tag}>[REDACTED]</{canonical_tag}>"));
                break;
            };
            let open_end = after_name + rel_end;
            if open_end > start && input.as_bytes()[open_end - 1] == b'/' {
                result.push_str(&format!("<{canonical_tag}/>"));
                pos = open_end + 1;
                continue;
            }
            result.push_str(&format!("<{canonical_tag}>[REDACTED]</{canonical_tag}>"));
            if let Some(rel_close) = lower[open_end + 1..].find(&close_tag) {
                pos = open_end + 1 + rel_close + close_tag.len();
            } else {
                break;
            }
        }
        result
    }

    fn redact_sensitive_text(input: &str) -> String {
        let mut out = Self::redact_xml_tag(input, "privatekey", "PrivateKey");
        out = Self::redact_xml_tag(&out, "certificate", "Certificate");

        let mut result = String::with_capacity(out.len());
        let mut pos = 0usize;
        while pos < out.len() {
            let Some(rel_begin) = out[pos..].find("-----BEGIN ") else {
                result.push_str(&out[pos..]);
                break;
            };
            let begin = pos + rel_begin;
            let after_begin = begin + 11;
            let Some(rel_hdr_end) = out[after_begin..].find("-----") else {
                result.push_str(&out[pos..after_begin]);
                pos = after_begin;
                continue;
            };
            let hdr_end = after_begin + rel_hdr_end;
            if !out[after_begin..hdr_end].contains("PRIVATE KEY") {
                result.push_str(&out[pos..after_begin]);
                pos = after_begin;
                continue;
            }
            result.push_str(&out[pos..begin]);
            result.push_str("[PRIVATE KEY REDACTED]");
            let Some(rel_end_marker) = out[hdr_end + 5..].find("-----END ") else {
                break;
            };
            let end_marker = hdr_end + 5 + rel_end_marker;
            if let Some(rel_end_finish) = out[end_marker + 9..].find("-----") {
                pos = end_marker + 9 + rel_end_finish + 5;
            } else {
                break;
            }
        }
        result
    }

    fn redact_json_node(node: &mut Value) {
        match node {
            Value::Object(map) => {
                for (k, v) in map.iter_mut() {
                    let kl = k.to_ascii_lowercase();
                    if matches!(
                        kl.as_str(),
                        "privatekey"
                            | "private_key"
                            | "certificate"
                            | "secret"
                            | "password"
                            | "token"
                            | "android_id"
                            | "serial"
                            | "ro.serialno"
                            | "ro.boot.serialno"
                            | "kaorios_keybox_xml"
                    ) {
                        *v = Value::String("[REDACTED]".to_string());
                    } else {
                        Self::redact_json_node(v);
                    }
                }
            }
            Value::Array(arr) => {
                for elem in arr {
                    Self::redact_json_node(elem);
                }
            }
            Value::String(s) => {
                *s = Self::redact_sensitive_text(s);
            }
            _ => {}
        }
    }

    fn redact_config_payload(engine: &str, raw: &str) -> String {
        if engine == "tee" {
            return Self::redact_sensitive_text(raw);
        }
        let Ok(mut parsed) = serde_json::from_str::<Value>(raw) else {
            return "[REDACTED INVALID JSON PAYLOAD]\n".to_string();
        };
        Self::redact_json_node(&mut parsed);
        let pretty = serde_json::to_string_pretty(&parsed)
            .unwrap_or_else(|_| "[REDACTED INVALID JSON PAYLOAD]".to_string());
        format!("{pretty}\n")
    }

    fn cmd_config_get(&self, engine: &str, redacted: bool) -> i32 {
        let Ok(filename) = Self::config_name(engine) else {
            eprintln!("Invalid engine: {engine}");
            return 1;
        };
        match NativeStore::open(&self.data_dir).and_then(|s| s.read(&filename, false)) {
            Ok(payload) => {
                if redacted {
                    print!("{}", Self::redact_config_payload(engine, &payload));
                } else {
                    print!("{payload}");
                }
                0
            }
            Err(e) => {
                eprintln!("{e}");
                1
            }
        }
    }

    fn cmd_diagnostics(&self) -> i32 {
        println!("KaoriosHelper Diagnostic Information");
        println!("-------------------------------------");
        println!("Module Directory: {}", self.mod_dir);
        println!("Data Directory:   {}", self.data_dir);
        println!("Version:          {}\n", self.get_version());

        println!("Configuration Files:");
        for entry in [
            "pif.json",
            "pif.json.lkg",
            "hma.json",
            "hma.json.lkg",
            "spoof.json",
            "spoof.json.lkg",
            "keybox.xml",
            "keybox.xml.lkg",
            "status.json",
        ] {
            let p = format!("{}/{entry}", self.data_dir);
            if let Ok(meta) = fs::metadata(&p) {
                println!(
                    "  • {entry} - {} bytes, mode 0{:o}",
                    meta.size(),
                    meta.mode() & 0o777
                );
            } else {
                println!("  • {entry} - [NOT FOUND]");
            }
        }
        println!("\nActive Marker Files:");
        for m in [
            "hma.disabled",
            "pif.disabled",
            "tee.enabled",
            "copg.enabled",
        ] {
            let p = format!("{}/{m}", self.data_dir);
            println!(
                "  • {m}: {}",
                if Path::new(&p).exists() {
                    "EXISTS"
                } else {
                    "ABSENT"
                }
            );
        }
        0
    }
}

fn main() -> ExitCode {
    let mut json_mode = false;
    let mut has_action = false;
    let mut action = "status".to_string();
    let mut args: Vec<String> = Vec::new();

    for arg in std::env::args().skip(1) {
        if arg == "--json" {
            json_mode = true;
        } else if !has_action {
            action = arg;
            has_action = true;
        } else {
            args.push(arg);
        }
    }

    let ctl = HelperCtl::new();

    let code = match action.as_str() {
        "status" => match ctl.cmd_status(json_mode) {
            Ok(c) => c,
            Err(e) => {
                eprintln!("{e}");
                1
            }
        },
        "version" => ctl.cmd_version(json_mode),
        "enable" => {
            if args.is_empty() {
                eprintln!("Usage: helperctl enable <hma|pif|tee|copg>");
                1
            } else {
                ctl.cmd_toggle(&args[0], true)
            }
        }
        "disable" => {
            if args.is_empty() {
                eprintln!("Usage: helperctl disable <hma|pif|tee|copg>");
                1
            } else {
                ctl.cmd_toggle(&args[0], false)
            }
        }
        "prepare-boot" if args.is_empty() => ctl.cmd_prepare_boot(),
        "reload" => {
            let eng = args.first().map(String::as_str).unwrap_or("all");
            ctl.cmd_reload(eng)
        }
        "verify" => {
            let mut eng = "all";
            let mut clear_mode = false;
            for a in &args {
                if a == "--clear" {
                    clear_mode = true;
                } else {
                    eng = a.as_str();
                }
            }
            ctl.cmd_verify(eng, clear_mode)
        }
        "config" => {
            if args.len() == 2 && args[0] == "sync" {
                ctl.cmd_config_sync(&args[1])
            } else if args.len() == 2 && args[0] == "migrate" {
                ctl.cmd_config_migrate(&args[1])
            } else if args.len() >= 2 && args[0] == "validate" {
                let src = args.get(2).map(String::as_str).unwrap_or("");
                ctl.cmd_config_validate(&args[1], src)
            } else if args.len() >= 2 && args[0] == "status" {
                ctl.cmd_config_validate(&args[1], "")
            } else if args.len() >= 3 && args[0] == "set" {
                ctl.publish_config(&args[1], &args[2], false)
            } else if args.len() >= 2 && args[0] == "rollback" {
                ctl.publish_config(&args[1], "", true)
            } else if args.len() >= 2 && args[0] == "summary" {
                ctl.cmd_config_get(&args[1], true)
            } else if args.len() >= 2 && args[0] == "get" {
                let mut redacted = false;
                let mut target_eng = String::new();
                let mut bad = false;
                for a in &args[1..] {
                    if a == "--redacted" {
                        redacted = true;
                    } else if target_eng.is_empty() {
                        target_eng = a.clone();
                    } else {
                        bad = true;
                        break;
                    }
                }
                if bad || target_eng.is_empty() {
                    eprintln!("Usage: helperctl config get [--redacted] <pif|hma|tee|copg|kaorios>");
                    1
                } else {
                    ctl.cmd_config_get(&target_eng, redacted)
                }
            } else {
                eprintln!("Usage: helperctl config {{set|get|summary|rollback|validate|status|sync|migrate}} <pif|hma|tee|copg|kaorios|all> [file]");
                1
            }
        }
        "diagnostics" => ctl.cmd_diagnostics(),
        _ => {
            eprintln!("Usage: helperctl {{status|version|enable|disable|reload|prepare-boot|verify|config|diagnostics}} [--json]");
            1
        }
    };

    ExitCode::from(code as u8)
}
