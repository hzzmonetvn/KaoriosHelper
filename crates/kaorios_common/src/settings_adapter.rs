// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use crate::native_store::validate_native_json;
use serde_json::{Map, Value};
use std::collections::{BTreeMap, BTreeSet};

const MAX_PAYLOAD_BYTES: usize = 16 * 1024 * 1024;

pub const SCALAR_BOOLEANS: &[&str] = &[
    "kaorios_spoof_photos",
    "kaorios_spoof_netflix",
    "kaorios_spoof_disney_plus",
    "kaorios_spoof_messenger_chathead",
    "kaorios_spoof_pixel_apps",
    "kaorios_spoof_meizu_music",
    "kaorios_spoof_gms",
    "kaorios_spoof_vending",
    "kaorios_spoof_gameprops",
    "kaorios_secure_flag",
    "kaorios_hide_app_work",
    "kaorios_hide_dev_work",
    "kaorios_secure_flag_work",
    "kaorios_keybox_enabled",
    "kaorios_keybox_apply_all",
    "kaorios_always_strong",
    "kaorios_advanced_features",
    "kaorios_hide_usb_charging",
];

fn get_setting<'a>(values: &'a BTreeMap<String, String>, key: &str) -> Result<Option<&'a str>, String> {
    let Some(raw) = values.get(key) else {
        return Ok(None);
    };
    let trimmed = raw.trim();
    if trimmed.is_empty() || trimmed == "null" {
        return Ok(None);
    }
    if trimmed.len() > MAX_PAYLOAD_BYTES {
        return Err("Settings payload too large".to_string());
    }
    Ok(Some(raw.as_str()))
}

fn parse_object(text: &str) -> Result<Map<String, Value>, String> {
    let v: Value =
        serde_json::from_str(text).map_err(|_| "Settings JSON must be an object".to_string())?;
    match v {
        Value::Object(m) => Ok(m),
        _ => Err("Settings JSON must be an object".to_string()),
    }
}

fn parse_boolean(text: &str) -> Result<bool, String> {
    match text.trim().to_ascii_lowercase().as_str() {
        "1" | "true" => Ok(true),
        "0" | "false" => Ok(false),
        _ => Err("Settings boolean is invalid".to_string()),
    }
}

pub fn validate_package(value: &str) -> Result<(), String> {
    if value.is_empty()
        || !value.contains('.')
        || value.starts_with('.')
        || value.ends_with('.')
        || value.contains("..")
        || !value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'.')
    {
        return Err("Settings package identifier is invalid".to_string());
    }
    Ok(())
}

fn parse_strings(val: &Value, packages: bool) -> Result<Vec<String>, String> {
    let arr = val
        .as_array()
        .ok_or_else(|| "Settings list is invalid".to_string())?;
    let mut out = Vec::with_capacity(arr.len());
    for item in arr {
        let s = item
            .as_str()
            .ok_or_else(|| "Settings list is invalid".to_string())?;
        if s.is_empty() {
            return Err("Settings list is invalid".to_string());
        }
        if packages {
            validate_package(s)?;
        }
        out.push(s.to_string());
    }
    Ok(out)
}

fn merge_object(
    target: &mut Map<String, Value>,
    key: &str,
    values: Map<String, Value>,
) -> Result<(), String> {
    let entry = target
        .entry(key.to_string())
        .or_insert_with(|| Value::Object(Map::new()));
    let obj = entry
        .as_object_mut()
        .ok_or_else(|| "Existing settings configuration is incompatible".to_string())?;
    for (k, v) in values {
        obj.insert(k, v);
    }
    Ok(())
}

pub fn parse_pif_settings(raw: &str) -> Result<Map<String, Value>, String> {
    let trimmed = raw.trim();
    let mut incoming = if trimmed.starts_with('{') {
        parse_object(raw)?
    } else {
        let mut map = Map::new();
        for line in raw.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let Some(eq) = line.find('=') else {
                return Err("Settings PIF properties are invalid".to_string());
            };
            if eq == 0 {
                return Err("Settings PIF properties are invalid".to_string());
            }
            let key = line[..eq].trim();
            let mut val = line[eq + 1..].trim();
            if let Some(comment) = val.find('#') {
                val = val[..comment].trim();
            }
            if key.is_empty() || val.is_empty() {
                return Err("Settings PIF properties are invalid".to_string());
            }
            map.insert(key.to_string(), Value::String(val.to_string()));
        }
        map
    };

    let keys: Vec<String> = incoming.keys().cloned().collect();
    for k in keys {
        if k.is_empty() {
            return Err("Settings PIF field is invalid".to_string());
        }
        let v = incoming.get(&k).unwrap().clone();
        if !(v.is_string() || v.is_i64() || v.is_u64() || v.is_boolean()) {
            return Err("Settings PIF field is invalid".to_string());
        }
        if k == "enabled" {
            if let Some(s) = v.as_str() {
                incoming.insert(k, Value::Bool(parse_boolean(s)?));
            }
        } else if matches!(
            k.as_str(),
            "spoofBuild" | "spoofProps" | "spoofProvider" | "spoofSignature"
        ) {
            if let Some(s) = v.as_str() {
                incoming.insert(k, Value::from(if parse_boolean(s)? { 1 } else { 0 }));
            }
        }
    }

    let serialized = Value::Object(incoming.clone()).to_string();
    if !validate_native_json("pif", &serialized) {
        return Err("Settings PIF field is invalid".to_string());
    }
    Ok(incoming)
}

fn adapt_copg(out: &mut Map<String, Value>, values: &BTreeMap<String, String>) -> Result<(), String> {
    let mut enabled: Option<bool> = None;
    if let Some(raw) = get_setting(values, "kaorios_gameprops_json")? {
        let input = parse_object(raw)?;
        for k in input.keys() {
            if !matches!(k.as_str(), "games" | "enabled" | "debug") {
                return Err("Settings COPG semantics unsupported".to_string());
            }
        }
        let games = input
            .get("games")
            .and_then(|v| v.as_object())
            .ok_or_else(|| "Settings games map is invalid".to_string())?;
        if let Some(en) = input.get("enabled") {
            let b = en
                .as_bool()
                .ok_or_else(|| "Settings boolean is invalid".to_string())?;
            enabled = Some(b);
        } else if !out.contains_key("enabled") {
            enabled = Some(!games.is_empty());
        }
        if let Some(dbg) = input.get("debug") {
            if dbg.as_bool() != Some(false) {
                return Err("Settings COPG semantics unsupported".to_string());
            }
        }

        let section = if out.contains_key("profiles") {
            "profiles"
        } else if out.get("packages").is_some_and(|v| v.is_object()) {
            "packages"
        } else {
            "profiles"
        };
        if !out.contains_key(section) {
            out.insert(section.to_string(), Value::Object(Map::new()));
        }
        if !out.get(section).is_some_and(|v| v.is_object()) {
            return Err("Existing settings configuration is incompatible".to_string());
        }

        for (pkg, prof_val) in games {
            validate_package(pkg)?;
            let prof_obj = prof_val
                .as_object()
                .ok_or_else(|| "Settings game profile is invalid".to_string())?;
            let mut mapped = Map::new();
            for (fk, fv) in prof_obj {
                let s = fv
                    .as_str()
                    .ok_or_else(|| "Settings game field is invalid".to_string())?;
                if fk == "HARDWARE" || fk == "ID" {
                    let prop_key = if fk == "HARDWARE" {
                        "ro.hardware"
                    } else {
                        "ro.build.id"
                    };
                    let props_entry = mapped
                        .entry("props".to_string())
                        .or_insert_with(|| Value::Object(Map::new()));
                    if let Some(props_obj) = props_entry.as_object_mut() {
                        props_obj.insert(prop_key.to_string(), Value::String(s.to_string()));
                    }
                    continue;
                }
                let target_key = match fk.as_str() {
                    "MODEL" => "model",
                    "BRAND" => "brand",
                    "MANUFACTURER" => "manufacturer",
                    "DEVICE" => "device",
                    "PRODUCT" => "product",
                    "FINGERPRINT" => "fingerprint",
                    "RELEASE" => "release",
                    _ => return Err("Settings COPG semantics unsupported".to_string()),
                };
                mapped.insert(target_key.to_string(), Value::String(s.to_string()));
            }
            let sec_obj = out.get_mut(section).and_then(|v| v.as_object_mut()).unwrap();
            merge_object(sec_obj, pkg, mapped)?;
        }
    }

    if let Some(toggle) = get_setting(values, "kaorios_spoof_gameprops")? {
        enabled = Some(parse_boolean(toggle)? && enabled.unwrap_or(true));
    }
    if let Some(en) = enabled {
        out.insert("enabled".to_string(), Value::Bool(en));
    }
    Ok(())
}

fn adapt_hma(out: &mut Map<String, Value>, raw: &str) -> Result<(), String> {
    let input = parse_object(raw)?;
    for k in input.keys() {
        if !matches!(
            k.as_str(),
            "configVersion"
                | "scope"
                | "templates"
                | "settingsTemplates"
                | "ignoredPackagesForPresets"
                | "installSourceSpoof"
                | "webViewProtection"
                | "detailLog"
        ) {
            return Err("Settings HMA semantics unsupported".to_string());
        }
    }
    if let Some(v) = input.get("configVersion") {
        if v.as_i64() != Some(1) {
            return Err("Settings HMA version unsupported".to_string());
        }
    }
    for flag in ["webViewProtection", "detailLog"] {
        if let Some(v) = input.get(flag) {
            if v.as_bool() != Some(false) {
                return Err("Settings HMA semantics unsupported".to_string());
            }
        }
    }
    for k in [
        "settingsTemplates",
        "ignoredPackagesForPresets",
        "installSourceSpoof",
    ] {
        if let Some(v) = input.get(k) {
            let valid_empty = if k == "settingsTemplates" {
                v.as_object().is_some_and(|m| m.is_empty())
            } else {
                v.as_array().is_some_and(|a| a.is_empty())
            };
            if !valid_empty {
                return Err("Settings HMA semantics unsupported".to_string());
            }
        }
    }

    let templates = match input.get("templates") {
        Some(v) => v
            .as_object()
            .ok_or_else(|| "Settings HMA templates are invalid".to_string())?
            .clone(),
        None => Map::new(),
    };
    for entry in templates.values() {
        let obj = entry
            .as_object()
            .ok_or_else(|| "Settings HMA templates are invalid".to_string())?;
        if obj.len() != 1 || !obj.contains_key("appList") {
            return Err("Settings HMA templates are invalid".to_string());
        }
        parse_strings(&obj["appList"], true)?;
    }

    let scope = match input.get("scope") {
        Some(v) => v
            .as_object()
            .ok_or_else(|| "Settings HMA scope is invalid".to_string())?
            .clone(),
        None => Map::new(),
    };

    let mut mapped = Map::new();
    for (caller, caller_val) in &scope {
        validate_package(caller)?;
        let mut targets = if caller_val.is_array() {
            parse_strings(caller_val, true)?
        } else {
            let rule = caller_val
                .as_object()
                .ok_or_else(|| "Settings HMA semantics unsupported".to_string())?;
            if !input.contains_key("webViewProtection") {
                return Err("Settings HMA semantics unsupported".to_string());
            }
            for k in rule.keys() {
                if !matches!(
                    k.as_str(),
                    "useWhitelist"
                        | "excludeSystemApps"
                        | "hideInstallationSource"
                        | "hideSystemInstallationSource"
                        | "excludeTargetInstallationSource"
                        | "extraAppList"
                        | "extraOppositeAppList"
                        | "applyTemplates"
                        | "applyPresets"
                        | "applySettingTemplates"
                        | "applySettingsPresets"
                ) {
                    return Err("Settings HMA semantics unsupported".to_string());
                }
            }
            for flag in [
                "useWhitelist",
                "hideInstallationSource",
                "hideSystemInstallationSource",
                "excludeTargetInstallationSource",
            ] {
                if let Some(v) = rule.get(flag) {
                    if v.as_bool() != Some(false) {
                        return Err("Settings HMA semantics unsupported".to_string());
                    }
                }
            }
            if let Some(v) = rule.get("excludeSystemApps") {
                if !v.is_boolean() {
                    return Err("Settings HMA rule is invalid".to_string());
                }
            }
            for list_key in [
                "extraOppositeAppList",
                "applyPresets",
                "applySettingTemplates",
                "applySettingsPresets",
            ] {
                if let Some(v) = rule.get(list_key) {
                    if !parse_strings(v, false)?.is_empty() {
                        return Err("Settings HMA semantics unsupported".to_string());
                    }
                }
            }
            let mut list = match rule.get("extraAppList") {
                Some(v) => parse_strings(v, true)?,
                None => Vec::new(),
            };
            if let Some(tpls) = rule.get("applyTemplates") {
                for name in parse_strings(tpls, false)? {
                    if caller == "com.google.android.gms"
                        || caller == "com.google.android.gsf"
                        || !templates.contains_key(&name)
                    {
                        return Err("Settings HMA semantics unsupported".to_string());
                    }
                    let members = parse_strings(&templates[&name]["appList"], true)?;
                    list.extend(members);
                }
            }
            list
        };
        targets.sort();
        targets.dedup();
        mapped.insert(
            caller.clone(),
            Value::Array(targets.into_iter().map(Value::String).collect()),
        );
    }

    if input.contains_key("scope") {
        merge_object(out, "scope", mapped)?;
    }
    if input.contains_key("configVersion") {
        out.insert("configVersion".to_string(), Value::from(1));
    }
    Ok(())
}

pub fn settings_keys(engine: &str) -> Result<Vec<String>, String> {
    match engine {
        "pif" => Ok(vec!["kaorios_pif_json".to_string()]),
        "copg" => Ok(vec![
            "kaorios_gameprops_json".to_string(),
            "kaorios_spoof_gameprops".to_string(),
        ]),
        "hma" => Ok(vec!["kaorios_hma_config".to_string()]),
        "tee" => Ok(vec!["kaorios_keybox_xml".to_string()]),
        "kaorios" => {
            let mut keys: Vec<String> = SCALAR_BOOLEANS.iter().map(|s| (*s).to_string()).collect();
            for k in [
                "kaorios_keybox_active_profile",
                "kaorios_keybox_apply_all_mode",
                "kaorios_security_patch",
                "kaorios_manager_package",
            ] {
                keys.push(k.to_string());
            }
            Ok(keys)
        }
        "all" => {
            let mut keys = BTreeSet::new();
            for name in ["pif", "copg", "hma", "tee", "kaorios"] {
                for k in settings_keys(name)? {
                    keys.insert(k);
                }
            }
            Ok(keys.into_iter().collect())
        }
        _ => Err("Settings engine unsupported".to_string()),
    }
}

pub fn validate_keybox_xml_structure(raw: &str) -> bool {
    let root = (raw.contains("<AndroidAttestation") && raw.contains("</AndroidAttestation>"))
        || (raw.contains("<KeyboxContent") && raw.contains("</KeyboxContent>"))
        || (raw.contains("<Keybox") && raw.contains("</Keybox>"));
    if !root {
        return false;
    }

    let mut key_open = None;
    let mut pos = 0usize;
    while let Some(rel) = raw[pos..].find("<Key") {
        let idx = pos + rel;
        let after = idx + 4;
        if let Some(&b) = raw.as_bytes().get(after) {
            if matches!(b, b'>' | b' ' | b'\t' | b'\r' | b'\n') {
                key_open = Some(idx);
                break;
            }
        }
        pos = after;
    }
    let Some(key_idx) = key_open else {
        return false;
    };
    let Some(end_rel) = raw[key_idx..].find("</Key>") else {
        return false;
    };
    let end_idx = key_idx + end_rel;
    if end_idx <= key_idx {
        return false;
    }

    let Some(priv_rel) = raw[key_idx..].find("<PrivateKey") else {
        return false;
    };
    let priv_idx = key_idx + priv_rel;
    let Some(priv_end_rel) = raw[key_idx..].find("</PrivateKey>") else {
        return false;
    };
    let priv_end_idx = key_idx + priv_end_rel;
    if priv_end_idx <= priv_idx || priv_end_idx > end_idx {
        return false;
    }

    let Some(cert_rel) = raw[key_idx..].find("<Certificate") else {
        return false;
    };
    let cert_idx = key_idx + cert_rel;
    let Some(cert_end_rel) = raw[key_idx..].find("</Certificate>") else {
        return false;
    };
    let cert_end_idx = key_idx + cert_end_rel;
    !(cert_end_idx <= cert_idx || cert_end_idx > end_idx)
}

pub fn adapt_settings(
    engine: &str,
    existing_bytes: &str,
    values: &BTreeMap<String, String>,
) -> Result<Option<String>, String> {
    if engine == "all" {
        return Err("Settings adapter requires one engine".to_string());
    }
    let keys = settings_keys(engine)?;
    let mut any_present = false;
    for k in &keys {
        if get_setting(values, k)?.is_some() {
            any_present = true;
            break;
        }
    }
    if !any_present {
        return Ok(None);
    }

    if engine == "tee" {
        let raw = get_setting(values, "kaorios_keybox_xml")?.unwrap();
        if !validate_keybox_xml_structure(raw) {
            return Err("Settings keybox structure is invalid".to_string());
        }
        return if raw == existing_bytes {
            Ok(None)
        } else {
            Ok(Some(raw.to_string()))
        };
    }

    let previous = parse_object(if existing_bytes.is_empty() {
        "{}"
    } else {
        existing_bytes
    })?;
    let mut out = previous.clone();

    match engine {
        "pif" => {
            let pif_map = parse_pif_settings(get_setting(values, "kaorios_pif_json")?.unwrap())?;
            for (k, v) in pif_map {
                out.insert(k, v);
            }
        }
        "copg" => adapt_copg(&mut out, values)?,
        "hma" => adapt_hma(&mut out, get_setting(values, "kaorios_hma_config")?.unwrap())?,
        "kaorios" => {
            let mut mapped = Map::new();
            for &key in SCALAR_BOOLEANS {
                if let Some(v) = get_setting(values, key)? {
                    mapped.insert(
                        key.to_string(),
                        Value::String(if parse_boolean(v)? { "1" } else { "0" }.to_string()),
                    );
                }
            }
            for key in [
                "kaorios_keybox_active_profile",
                "kaorios_keybox_apply_all_mode",
                "kaorios_security_patch",
                "kaorios_manager_package",
            ] {
                if let Some(v) = get_setting(values, key)? {
                    let text = v.trim().to_string();
                    if key == "kaorios_keybox_active_profile" && text != "hw" && text != "rkp" {
                        return Err("Settings profile is invalid".to_string());
                    }
                    if key == "kaorios_keybox_apply_all_mode"
                        && text != "auto"
                        && text != "leaf"
                        && text != "gen"
                    {
                        return Err("Settings mode is invalid".to_string());
                    }
                    if key == "kaorios_manager_package" {
                        validate_package(&text)?;
                    }
                    mapped.insert(key.to_string(), Value::String(text));
                }
            }
            merge_object(&mut out, "settingsGlobal", mapped)?;
            if !out.contains_key("schemaVersion") {
                out.insert("schemaVersion".to_string(), Value::from(1));
            }
        }
        _ => return Err("Settings engine unsupported".to_string()),
    }

    let compact = Value::Object(out.clone()).to_string();
    if !validate_native_json(engine, &compact) {
        return Err("Adapted settings configuration is invalid".to_string());
    }
    if out == previous {
        Ok(None)
    } else {
        let pretty = serde_json::to_string_pretty(&Value::Object(out))
            .map_err(|_| "Adapted settings configuration is invalid".to_string())?;
        Ok(Some(format!("{pretty}\n")))
    }
}
