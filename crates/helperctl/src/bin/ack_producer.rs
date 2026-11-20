// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 HzzMonet / Kaorios Project
// Test fixture binary for live native ACK production.

use kaorios_common::ack_protocol::{AckEventType, RuntimeAck};
use kaorios_common::runtime_snapshot::{
    bind_runtime_ack_to_snapshot, read_runtime_config_snapshot,
    write_runtime_ack_for_current_config,
};
use std::io::{self, BufRead, Write};
use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        return ExitCode::from(1);
    }
    let data_dir = &args[1];
    let event_str = &args[2];

    let Ok(snapshot) = read_runtime_config_snapshot(data_dir, "copg", "spoof.json") else {
        return ExitCode::from(2);
    };
    let Some(ev) = AckEventType::from_str_strict(event_str) else {
        return ExitCode::from(2);
    };
    let mut ack = RuntimeAck {
        engine: "copg".to_string(),
        ..Default::default()
    };
    ack.set_event(ev);
    if !bind_runtime_ack_to_snapshot(&mut ack, &snapshot)
        || !write_runtime_ack_for_current_config(data_dir, &ack)
    {
        return ExitCode::from(2);
    }
    println!("ready");
    let _ = io::stdout().flush();

    let stdin = io::stdin();
    for line in stdin.lock().lines() {
        let Ok(cmd) = line else {
            break;
        };
        let cmd = cmd.trim();
        if cmd == "fail" {
            ack.set_event(AckEventType::ApplyFailed);
            ack.init_result = -1;
        } else if cmd == "observe" {
            ack.set_event(AckEventType::HookObserved);
            ack.init_result = 0;
        } else if cmd != "repeat" {
            return ExitCode::from(1);
        }
        if write_runtime_ack_for_current_config(data_dir, &ack) {
            println!("published");
        } else {
            println!("rejected");
        }
        let _ = io::stdout().flush();
    }
    ExitCode::SUCCESS
}
