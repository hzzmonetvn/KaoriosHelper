/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * Pinned Upstream Commit: frknkrc44/HMA-OSS @ 1fe86ce2b4a6f21197e6e92d15e054d609b7d77c
 */

package org.frknkrc44.hma_oss.zygote;

import java.util.concurrent.atomic.AtomicInteger;

public class ZygoteEntry {
    private static final String TAG = "[Kaorios/HmaZygote]";
    private static final AtomicInteger sHookState = new AtomicInteger(HmaConstants.STATE_DEX_LOADED);
    private static final HmaPolicyManager sPolicyManager = new HmaPolicyManager();
    private static final ZygoteEntry sInstance = new ZygoteEntry();

    public static void main() {
        main(new String[0]);
    }

    public static void main(String[] args) {
        System.out.println(TAG + " Pinned upstream frknkrc44/HMA-OSS @" + HmaConstants.UPSTREAM_COMMIT);
        System.out.println(TAG + " Initializing caller-aware package hiding runtime inside system_server");

        sHookState.set(HmaConstants.STATE_ENTRYPOINT_EXECUTED);

        // Load configuration
        boolean configOk = sPolicyManager.loadConfig("/data/adb/modules/kaorioshelper/data/hma.json");
        if (!configOk) {
            configOk = sPolicyManager.loadConfig("/data/adb/modules/kaorioshelper/config/hma.json");
        }
        System.out.println(TAG + " Configuration loaded: " + configOk + " (global hidden: " + sPolicyManager.getGlobalHiddenPackages().size() + ")");

        // Initialize hooking layer
        sHookState.set(HmaConstants.STATE_ART_HOOK_INIT);
        boolean pmsHookOk = PackageManagerHook.install(sPolicyManager, sInstance);
        if (pmsHookOk) {
            sHookState.set(HmaConstants.STATE_PMS_HOOK_INSTALLED);
            System.out.println(TAG + " PackageManager hook installed successfully in system_server");
        } else {
            System.err.println(TAG + " PackageManager hook could not be installed in current process context");
        }
    }

    public void notifyCallerPolicyApplied() {
        sHookState.compareAndSet(HmaConstants.STATE_PMS_HOOK_INSTALLED, HmaConstants.STATE_CALLER_POLICY_APPLIED);
    }

    public void notifyResponseFiltered() {
        sHookState.set(HmaConstants.STATE_RESPONSE_FILTERED);
    }

    public static int getHookState() {
        return sHookState.get();
    }

    public static String getHookStateName() {
        return HmaConstants.stateToString(sHookState.get());
    }

    public static long getFilterCount() {
        return sPolicyManager.getFilterCount();
    }

    public static boolean shouldHide(int callingUid, String targetPkg) {
        return sPolicyManager.shouldHide(callingUid, null, targetPkg);
    }
}
