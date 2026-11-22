// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project

use std::env;
use std::path::PathBuf;

fn main() {
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
    if target_os != "android" {
        return;
    }

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root_dir = manifest_dir.parent().unwrap().parent().unwrap();
    let sh_dir = root_dir.join("third_party/shadowhook");
    let export_map = root_dir.join("scripts/zygisk_export.map");

    println!("cargo:rerun-if-changed={}", export_map.display());
    println!("cargo:rerun-if-changed={}", sh_dir.display());

    let mut build = cc::Build::new();
    build
        .opt_level(3)
        .pic(true)
        .flag("-fvisibility=hidden")
        .include(sh_dir.join("include"))
        .include(&sh_dir)
        .include(sh_dir.join("common"))
        .include(sh_dir.join("third_party/xdl"))
        .include(sh_dir.join("third_party/bsd"))
        .include(sh_dir.join("third_party/lss"));

    let mut files = vec![
        "shadowhook.c",
        "sh_elf.c",
        "sh_enter.c",
        "sh_hub.c",
        "sh_island.c",
        "sh_jni.c",
        "sh_linker.c",
        "sh_recorder.c",
        "sh_safe.c",
        "sh_switch.c",
        "sh_task.c",
        "sh_xdl.c",
        "common/bytesig.c",
        "common/sh_errno.c",
        "common/sh_log.c",
        "common/sh_ref.c",
        "common/sh_trampo.c",
        "common/sh_util.c",
        "third_party/xdl/xdl.c",
        "third_party/xdl/xdl_iterate.c",
        "third_party/xdl/xdl_linker.c",
        "third_party/xdl/xdl_lzma.c",
        "third_party/xdl/xdl_util.c",
    ];

    if target_arch == "aarch64" {
        build.include(sh_dir.join("arch/arm64"));
        files.extend([
            "arch/arm64/sh_a64.c",
            "arch/arm64/sh_inst.c",
            "arch/arm64/sh_glue.S",
        ]);
        println!("cargo:rustc-cdylib-link-arg=-Wl,-soname,arm64-v8a.so");
        println!("cargo:rustc-cdylib-link-arg=-Wl,-z,max-page-size=16384");
    } else if target_arch == "arm" {
        build.include(sh_dir.join("arch/arm"));
        build.flag("-mthumb");
        files.extend([
            "arch/arm/sh_a32.c",
            "arch/arm/sh_t16.c",
            "arch/arm/sh_t32.c",
            "arch/arm/sh_txx.c",
            "arch/arm/sh_inst.c",
            "arch/arm/sh_glue.S",
        ]);
        println!("cargo:rustc-cdylib-link-arg=-Wl,-soname,armeabi-v7a.so");
    }

    for rel in files {
        build.file(sh_dir.join(rel));
    }
    build.compile("shadowhook");

    println!(
        "cargo:rustc-cdylib-link-arg=-Wl,--version-script={}",
        export_map.display()
    );
    println!("cargo:rustc-cdylib-link-arg=-Wl,--gc-sections");
    println!("cargo:rustc-link-lib=log");
}
