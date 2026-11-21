// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use kaorios_common::ack_protocol::{
    bind_runtime_ack_to_current_process, is_pre_argv0_zygote_cmdline, parse_runtime_ack,
    runtime_ack_boot_id, runtime_ack_now_millis, validate_runtime_ack, AckEventType, RuntimeAck,
};
use kaorios_common::companion_ipc::{CompanionConnector, CompanionSession};
use kaorios_common::companion_server::handle_companion_session_with_dir;
use kaorios_common::native_store::{validate_native_json, Changes, NativeStore};
use kaorios_common::runtime_snapshot::read_runtime_config_snapshot;
use kaorios_common::settings_adapter::{adapt_settings, settings_keys};
use std::collections::BTreeMap;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use std::thread;
use std::time::{SystemTime, UNIX_EPOCH};

fn temp_dir(prefix: &str) -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let p = std::env::temp_dir().join(format!("{prefix}_{}_{}", std::process::id(), nanos));
    fs::create_dir_all(&p).unwrap();
    fs::set_permissions(&p, fs::Permissions::from_mode(0o700)).unwrap();
    p
}

#[test]
fn ack_rejects_duplicate_keys_and_deep_nesting() {
    let dup = r#"{"schemaVersion":1,"schemaVersion":1,"protocolVersion":1,"engine":"pif","processName":"com.google.android.gms","pid":1,"uid":1000,"eventType":"PROCESS_LOADED","configGeneration":1,"initResult":0,"errorCode":0,"timestamp":1,"detail":"ok"}"#;
    assert!(parse_runtime_ack(dup).is_none());

    let deep = format!("{}{}{}", "[".repeat(18), "1", "]".repeat(18));
    assert!(parse_runtime_ack(&deep).is_none());
}

#[test]
fn ack_ownership_policy_enforced() {
    let mut ack = RuntimeAck {
        schema_version: 1,
        protocol_version: 1,
        engine: "hma".to_string(),
        process_name: "com.google.android.gms".to_string(),
        pid: 123,
        uid: 1000,
        event_type: AckEventType::ProcessLoaded.as_str().to_string(),
        ..Default::default()
    };
    assert!(validate_runtime_ack(&ack).is_err());

    ack.process_name = "system_server".to_string();
    assert!(validate_runtime_ack(&ack).is_ok());

    ack.engine = "copg".to_string();
    assert!(validate_runtime_ack(&ack).is_err());

    ack.process_name = "com.miHoYo.GenshinImpact".to_string();
    assert!(validate_runtime_ack(&ack).is_ok());
}

#[test]
fn native_store_transaction_and_journal() {
    let dir = temp_dir("kaorios_store_test");
    let dir_str = dir.to_string_lossy().into_owned();
    let store = NativeStore::open(&dir_str).unwrap();

    let mut changes = Changes::new();
    changes.insert("pif.json".to_string(), Some("{\"MODEL\":\"Pixel 8\"}\n".to_string()));
    changes.insert("pif.stored_gen".to_string(), Some("1\n".to_string()));
    store.transaction(&changes).unwrap();

    assert_eq!(store.read("pif.json", false).unwrap(), "{\"MODEL\":\"Pixel 8\"}\n");
    assert_eq!(store.read("pif.stored_gen", false).unwrap(), "1\n");
    drop(store);

    let snap = read_runtime_config_snapshot(&dir_str, "pif", "pif.json").unwrap();
    assert!(!snap.config_hash.is_empty());

    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn settings_adapter_maps_pif_copg_and_hma() {
    let mut pif_vals = BTreeMap::new();
    pif_vals.insert(
        "kaorios_pif_json".to_string(),
        "MODEL=Pixel 8 Pro\nspoofProvider=true\n".to_string(),
    );
    let out = adapt_settings("pif", "", &pif_vals).unwrap().unwrap();
    assert!(validate_native_json("pif", &out));
    assert!(out.contains("\"MODEL\": \"Pixel 8 Pro\""));

    let mut copg_vals = BTreeMap::new();
    copg_vals.insert(
        "kaorios_gameprops_json".to_string(),
        r#"{"games":{"com.example.game":{"MODEL":"ROG 8","HARDWARE":"qcom","ID":"UP1A"}}}"#
            .to_string(),
    );
    let copg_out = adapt_settings("copg", "", &copg_vals).unwrap().unwrap();
    assert!(validate_native_json("copg", &copg_out));
    assert!(copg_out.contains("\"ro.hardware\": \"qcom\""));
    assert!(copg_out.contains("\"ro.build.id\": \"UP1A\""));

    assert!(!settings_keys("all").unwrap().is_empty());

    let mut dummy_ack = RuntimeAck {
        engine: "pif".to_string(),
        ..Default::default()
    };
    let _ = bind_runtime_ack_to_current_process(&mut dummy_ack, 1, b"{}", "0123456789abcdef0123456789abcdef");
}

struct TestPairConnector(libc::c_int);

impl CompanionConnector for TestPairConnector {
    fn connect_companion(&self) -> libc::c_int {
        self.0
    }

    fn exempt_fd(&self, fd: libc::c_int) -> bool {
        fd >= 0
    }
}

#[test]
fn ack_pre_argv0_zygote_cmdline_helper() {
    assert!(is_pre_argv0_zygote_cmdline("zygote64"));
    assert!(is_pre_argv0_zygote_cmdline("zygote"));
    assert!(is_pre_argv0_zygote_cmdline("usap64"));
    assert!(is_pre_argv0_zygote_cmdline("<pre-initialized>"));
    assert!(is_pre_argv0_zygote_cmdline("/system/bin/app_process64"));
    assert!(!is_pre_argv0_zygote_cmdline("com.example.game"));
    assert!(!is_pre_argv0_zygote_cmdline("system_server"));
}

#[test]
fn companion_fetch_state_and_keepalive_over_socketpair() {
    let dir = temp_dir("kaorios_companion_fetch");
    let dir_str = dir.to_string_lossy().into_owned();
    let boot = runtime_ack_boot_id();
    let req_id = "0123456789abcdef0123456789abcdef";
    let not_before = runtime_ack_now_millis().saturating_sub(5).max(1);

    let store = NativeStore::open(&dir_str).unwrap();
    let mut changes = Changes::new();
    changes.insert("hma.json".to_string(), Some("{\"scope\":{}}\n".to_string()));
    changes.insert("hma.stored_gen".to_string(), Some("1\n".to_string()));
    changes.insert("hma.requested_gen".to_string(), Some("1\n".to_string()));
    changes.insert(
        "hma.request.json".to_string(),
        Some(format!(
            "{{\"requestId\":\"{req_id}\",\"bootId\":\"{boot}\",\"notBeforeMillis\":{not_before}}}"
        )),
    );
    changes.insert("pif.json".to_string(), Some("{\"MODEL\":\"Pixel 9\"}\n".to_string()));
    changes.insert("tee.enabled".to_string(), Some(String::new()));
    changes.insert("keybox.xml".to_string(), Some("<Keybox/>".to_string()));
    store.transaction(&changes).unwrap();
    drop(store);

    let mut fds = [-1 as libc::c_int; 2];
    assert_eq!(
        unsafe { libc::socketpair(libc::AF_UNIX, libc::SOCK_STREAM, 0, fds.as_mut_ptr()) },
        0
    );
    let client_fd = fds[0];
    let server_fd = fds[1];
    let server_dir = dir_str.clone();
    let server_thread = thread::spawn(move || {
        handle_companion_session_with_dir(server_fd, &server_dir, 200);
    });

    let conn = TestPairConnector(client_fd);
    let mut session = CompanionSession::new();
    session.pre_specialize(Some(&conn));
    assert!(session.has_retained_socket());

    let copg_state = session.fetch_engine_state(None, "copg").unwrap();
    assert!(!copg_state.enabled);

    let pif_state = session.fetch_engine_state(None, "pif").unwrap();
    assert!(pif_state.enabled);
    assert_eq!(pif_state.snapshot.bytes, "{\"MODEL\":\"Pixel 9\"}\n");
    assert_eq!(pif_state.boot_id, boot);

    let tee_state = session.fetch_engine_state(None, "tee").unwrap();
    assert!(tee_state.enabled);
    assert_eq!(tee_state.snapshot.bytes, "<Keybox/>");

    let hma_state = session.fetch_engine_state(None, "hma").unwrap();
    assert!(hma_state.enabled);
    assert_eq!(hma_state.snapshot.generation, 1);
    assert_eq!(hma_state.snapshot.request_id, req_id);

    session.close_session();
    server_thread.join().unwrap();
    let _ = fs::remove_dir_all(&dir);
}
