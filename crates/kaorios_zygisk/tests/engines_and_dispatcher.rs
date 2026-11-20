// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use kaorios_zygisk::dispatcher::KaoriosDispatcher;
use kaorios_zygisk::engine::Engine;
use kaorios_zygisk::engines::copg::CopgEngine;
use kaorios_zygisk::engines::hma::HmaEngine;
use kaorios_zygisk::engines::pif::PifEngine;
use kaorios_zygisk::engines::tee::TeeEngine;
use std::collections::BTreeMap;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
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
fn dispatcher_registers_all_four_engines() {
    let dir = temp_dir("kaorios_disp");
    let d = dir.to_string_lossy().into_owned();
    let mut disp = KaoriosDispatcher::new();
    disp.register_engine(Box::new(HmaEngine::new(d.clone())));
    disp.register_engine(Box::new(PifEngine::new(d.clone())));
    disp.register_engine(Box::new(TeeEngine::new(d.clone())));
    disp.register_engine(Box::new(CopgEngine::new(d)));
    disp.on_load(None, std::ptr::null_mut());
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn hma_scope_whitelist_and_opposites() {
    let dir = temp_dir("kaorios_hma");
    let d = dir.to_string_lossy().into_owned();
    fs::write(
        dir.join("hma.json"),
        r#"{
            "scope": {
                "com.example.bank": {
                    "useWhitelist": false,
                    "extraAppList": ["com.other.app"],
                    "extraOppositeAppList": ["com.topjohnwu.magisk"]
                }
            }
        }"#,
    )
    .unwrap();
    let mut hma = HmaEngine::new(d);
    assert!(hma.is_enabled());
    assert!(hma.should_handle_server());
    assert!(hma.load_config());
    assert!(hma.should_hide(10123, &["com.example.bank"], "com.other.app"));
    assert!(!hma.should_hide(10123, &["com.example.bank"], "com.topjohnwu.magisk"));
    assert!(!hma.should_hide(1000, &["com.example.bank"], "com.other.app"));
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn pif_normalizes_security_patch_and_tee_coexistence() {
    let dir = temp_dir("kaorios_pif");
    let d = dir.to_string_lossy().into_owned();
    fs::write(
        dir.join("pif.json"),
        r#"{
            "MODEL": "Pixel 8",
            "SECURITY_PATCH": "20240505",
            "spoofProvider": true
        }"#,
    )
    .unwrap();
    let mut pif = PifEngine::new(d);
    assert!(pif.is_enabled());
    assert!(pif.should_handle_app("com.google.android.gms.unstable", 10100));
    assert!(!pif.should_handle_app("com.example.game", 10100));

    let mut props = BTreeMap::new();
    let mut sp = 0;
    assert!(pif.load_config(&mut props, &mut sp));
    assert_eq!(props.get("SECURITY_PATCH").map(String::as_str), Some("2024-05-05"));
    assert_eq!(sp, 1);
    assert_eq!(
        PifEngine::resolve_spoofed_property("ro.product.model", ""),
        Some("Pixel 8".to_string())
    );
    assert_eq!(
        PifEngine::resolve_spoofed_property("ro.build.version.security_patch", ""),
        Some("2024-05-05".to_string())
    );
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn tee_validates_keybox_structure() {
    let dir = temp_dir("kaorios_tee");
    let d = dir.to_string_lossy().into_owned();
    fs::write(dir.join("tee.enabled"), "").unwrap();
    fs::write(
        dir.join("keybox.xml"),
        "<AndroidAttestation><Keybox DeviceID=\"dev1\"><Key algorithm=\"ecdsa\"><PrivateKey>abc</PrivateKey><CertificateChain><Certificate>cert</Certificate></CertificateChain></Key></Keybox></AndroidAttestation>",
    )
    .unwrap();
    let tee = TeeEngine::new(d);
    assert!(tee.is_enabled());
    assert!(tee.has_valid_keybox());
    assert!(!tee.is_attestation_simulation_active());
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn copg_parses_profiles_and_blocks_gms() {
    let dir = temp_dir("kaorios_copg");
    let d = dir.to_string_lossy().into_owned();
    fs::write(dir.join("copg.enabled"), "").unwrap();
    fs::write(
        dir.join("spoof.json"),
        r#"{
            "enabled": true,
            "profiles": {
                "com.example.game": {
                    "model": "ASUS_AI2401_A",
                    "brand": "asus",
                    "cpu_spoof": true
                },
                "com.google.android.gms": {
                    "model": "Evil"
                }
            }
        }"#,
    )
    .unwrap();
    let copg = CopgEngine::new(d);
    assert!(copg.is_enabled());
    assert!(copg.reload_config_if_needed(true));
    let (info, flags) = copg.get_package_config("com.example.game").unwrap();
    assert_eq!(info.model, "ASUS_AI2401_A");
    assert!(flags.needs_device_spoof);
    assert!(flags.needs_plt_hook);
    assert!(flags.needs_cpu_mount);
    assert!(copg.get_package_config("com.google.android.gms").is_none());
    let _ = fs::remove_dir_all(&dir);
}
