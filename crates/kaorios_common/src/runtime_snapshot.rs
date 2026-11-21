// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use crate::ack_protocol::{
    bind_runtime_ack_to_current_process, publish_runtime_ack_with_bootstrap, runtime_ack_boot_id,
    runtime_ack_has_live_identity_with_bootstrap, runtime_ack_matches_current_with_bootstrap,
    runtime_ack_now_millis, validate_runtime_ack, RuntimeAck,
};
use crate::config_identity::config_sha256;
use crate::native_store::NativeStore;
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeConfigSnapshot {
    pub bytes: String,
    pub config_hash: String,
    pub request_id: String,
    pub boot_id: String,
    pub generation: u64,
    pub not_before_millis: u64,
}

impl RuntimeConfigSnapshot {
    pub fn same_identity(&self, other: &Self) -> bool {
        self.bytes == other.bytes
            && self.generation == other.generation
            && self.request_id == other.request_id
            && self.boot_id == other.boot_id
            && self.not_before_millis == other.not_before_millis
    }
}

pub fn runtime_snapshot_generation(text: &str) -> u64 {
    let trimmed_end = text.trim_end_matches([' ', '\r', '\n', '\t']);
    if trimmed_end.is_empty() || !trimmed_end.bytes().all(|b| b.is_ascii_digit()) {
        return 0;
    }
    trimmed_end.parse::<u64>().unwrap_or(0)
}

pub fn read_runtime_config_snapshot(
    data_dir: &str,
    engine: &str,
    filename: &str,
) -> Result<RuntimeConfigSnapshot, String> {
    let canonical = match engine {
        "hma" | "pif" => format!("{engine}.json"),
        "copg" => "spoof.json".to_string(),
        _ => return Err("Invalid runtime snapshot engine".to_string()),
    };
    if filename != canonical
        && !(engine == "copg" && filename == "COPG.json")
        && !(engine == "pif" && filename == "pif.prop")
    {
        return Err("Invalid runtime snapshot filename".to_string());
    }

    let store = NativeStore::open(data_dir)?;
    let mut snapshot = RuntimeConfigSnapshot::default();
    let mut legacy_fallback = false;
    if engine == "pif"
        && filename == canonical
        && !store.exists(filename)?
        && store.exists("pif.prop")?
    {
        legacy_fallback = true;
        snapshot.bytes = store.read("pif.prop", false)?;
    } else {
        snapshot.bytes = store.read(filename, engine == "copg" && filename == canonical)?;
        if engine == "copg" && filename == canonical && snapshot.bytes.is_empty() {
            legacy_fallback = true;
            snapshot.bytes = store.read("COPG.json", false)?;
        }
    }
    snapshot.config_hash = config_sha256(snapshot.bytes.as_bytes());
    if filename != canonical || legacy_fallback {
        return Ok(snapshot);
    }

    let stored = runtime_snapshot_generation(&store.read(&format!("{engine}.stored_gen"), true)?);
    let requested =
        runtime_snapshot_generation(&store.read(&format!("{engine}.requested_gen"), true)?);
    let req_text = store.read(&format!("{engine}.request.json"), true)?;
    let Ok(request) = serde_json::from_str::<Value>(&req_text) else {
        return Ok(snapshot);
    };
    if stored == 0 || stored != requested {
        return Ok(snapshot);
    }
    let Some(obj) = request.as_object() else {
        return Ok(snapshot);
    };
    let Some(token) = obj.get("requestId").and_then(|v| v.as_str()) else {
        return Ok(snapshot);
    };
    if token.len() != 32
        || !token
            .bytes()
            .all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
    {
        return Ok(snapshot);
    }
    let Some(boot) = obj.get("bootId").and_then(|v| v.as_str()) else {
        return Ok(snapshot);
    };
    let Some(not_before) = obj.get("notBeforeMillis").and_then(|v| v.as_u64()) else {
        return Ok(snapshot);
    };
    let now = runtime_ack_now_millis();
    if boot.is_empty() || boot != runtime_ack_boot_id() || not_before == 0 || not_before > now {
        return Ok(snapshot);
    }

    snapshot.generation = stored;
    snapshot.request_id = token.to_string();
    snapshot.boot_id = boot.to_string();
    snapshot.not_before_millis = not_before;
    Ok(snapshot)
}

pub fn bind_runtime_ack_to_snapshot(
    ack: &mut RuntimeAck,
    snapshot: &RuntimeConfigSnapshot,
) -> bool {
    let local_boot = runtime_ack_boot_id();
    if snapshot.generation == 0
        || snapshot.boot_id.is_empty()
        || (!local_boot.is_empty() && snapshot.boot_id != local_boot)
        || snapshot.config_hash != config_sha256(snapshot.bytes.as_bytes())
    {
        return false;
    }
    ack.boot_id = snapshot.boot_id.clone();
    if !bind_runtime_ack_to_current_process(
        ack,
        snapshot.generation,
        snapshot.bytes.as_bytes(),
        &snapshot.request_id,
    ) {
        return false;
    }
    ack.timestamp >= snapshot.not_before_millis
}

pub fn write_runtime_ack_for_current_config(data_dir: &str, ack: &RuntimeAck) -> bool {
    write_runtime_ack_for_current_config_with_bootstrap(data_dir, ack, false)
}

pub fn write_runtime_ack_for_current_config_with_bootstrap(
    data_dir: &str,
    ack: &RuntimeAck,
    allow_zygote_bootstrap: bool,
) -> bool {
    if validate_runtime_ack(ack).is_err()
        || !runtime_ack_has_live_identity_with_bootstrap(ack, allow_zygote_bootstrap)
    {
        return false;
    }
    let Ok(store) = NativeStore::open(data_dir) else {
        return false;
    };
    let filename = match ack.engine.as_str() {
        "copg" => "spoof.json".to_string(),
        "tee" => "keybox.xml".to_string(),
        e => format!("{e}.json"),
    };
    let enabled = match ack.engine.as_str() {
        "hma" | "pif" => !store
            .exists(&format!("{}.disabled", ack.engine))
            .unwrap_or(true),
        _ => store
            .exists(&format!("{}.enabled", ack.engine))
            .unwrap_or(false),
    };
    if !enabled {
        return false;
    }
    let Ok(req_text) = store.read(&format!("{}.request.json", ack.engine), false) else {
        return false;
    };
    let Ok(request) = serde_json::from_str::<Value>(&req_text) else {
        return false;
    };
    let generation = format!("{}\n", ack.config_generation);
    if store
        .read(&format!("{}.stored_gen", ack.engine), false)
        .ok()
        .as_deref()
        != Some(generation.as_str())
        || store
            .read(&format!("{}.requested_gen", ack.engine), false)
            .ok()
            .as_deref()
            != Some(generation.as_str())
    {
        return false;
    }
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
    if boot != runtime_ack_boot_id() || not_before == 0 || ack.timestamp < not_before {
        return false;
    }
    let Ok(cfg_bytes) = store.read(&filename, false) else {
        return false;
    };
    if !runtime_ack_matches_current_with_bootstrap(
        ack,
        &ack.engine,
        ack.config_generation,
        cfg_bytes.as_bytes(),
        token,
        allow_zygote_bootstrap,
    ) {
        return false;
    }
    publish_runtime_ack_with_bootstrap(
        &format!("{data_dir}/runtime"),
        ack,
        token,
        allow_zygote_bootstrap,
    )
}
