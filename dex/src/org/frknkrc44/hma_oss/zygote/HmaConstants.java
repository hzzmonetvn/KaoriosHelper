/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * Pinned Upstream Commit: frknkrc44/HMA-OSS @ 1fe86ce2b4a6f21197e6e92d15e054d609b7d77c
 */

package org.frknkrc44.hma_oss.zygote;

public final class HmaConstants {
    private HmaConstants() {}

    public static final String UPSTREAM_COMMIT = "1fe86ce2b4a6f21197e6e92d15e054d609b7d77c";
    public static final String UPSTREAM_REPO = "https://github.com/frknkrc44/HMA-OSS";

    public static final int UID_ROOT = 0;
    public static final int UID_SYSTEM = 1000;
    public static final int FIRST_APPLICATION_UID = 10000;

    // Distinct hook installation verification stages
    public static final int STATE_NOT_STARTED = 0;
    public static final int STATE_DEX_EXISTS = 1;
    public static final int STATE_DEX_LOADED = 2;
    public static final int STATE_ENTRYPOINT_EXECUTED = 3;
    public static final int STATE_ART_HOOK_INIT = 4;
    public static final int STATE_PMS_HOOK_INSTALLED = 5;
    public static final int STATE_CALLER_POLICY_APPLIED = 6;
    public static final int STATE_RESPONSE_FILTERED = 7;

    public static String stateToString(int state) {
        switch (state) {
            case STATE_NOT_STARTED: return "NOT_STARTED";
            case STATE_DEX_EXISTS: return "DEX_EXISTS";
            case STATE_DEX_LOADED: return "DEX_LOADED";
            case STATE_ENTRYPOINT_EXECUTED: return "ENTRYPOINT_EXECUTED";
            case STATE_ART_HOOK_INIT: return "ART_HOOK_INIT";
            case STATE_PMS_HOOK_INSTALLED: return "PMS_HOOK_INSTALLED";
            case STATE_CALLER_POLICY_APPLIED: return "CALLER_POLICY_APPLIED";
            case STATE_RESPONSE_FILTERED: return "RESPONSE_FILTERED";
            default: return "UNKNOWN";
        }
    }
}
