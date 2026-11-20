/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * Pinned Upstream Commit: frknkrc44/HMA-OSS @ 1fe86ce2b4a6f21197e6e92d15e054d609b7d77c
 */

package org.frknkrc44.hma_oss.zygote;

import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

public class ZygoteEntry {
    private static final String TAG = "[Kaorios/HmaZygote]";
    private static final AtomicInteger sHookState = new AtomicInteger(HmaConstants.STATE_DEX_LOADED);
    private static final AtomicBoolean sBootstrapRunning = new AtomicBoolean(false);
    private static final AtomicBoolean sObservedReported = new AtomicBoolean(false);
    private static final HmaPolicyManager sPolicyManager = new HmaPolicyManager();
    private static final ZygoteEntry sInstance = new ZygoteEntry();

    // Optional JNI callback registered by HmaEngine when running inside native Zygisk runtime
    private static native void nativeOnHookStateChanged(int state, String detail);

    private static void reportNativeState(int state, String detail) {
        try {
            nativeOnHookStateChanged(state, detail);
        } catch (UnsatisfiedLinkError ignored) {
            // Normal in standalone JVM unit tests or when native callback is unregistered
        } catch (Throwable ignored) {}
    }

    /**
     * Primary entrypoint invoked from native C++ postServerSpecialize via JNI.
     * Receives the in-memory configuration snapshot directly, avoiding DAC 0700 denials.
     */
    public static void init(String configJson) {
        System.out.println(TAG + " Pinned upstream frknkrc44/HMA-OSS @" + HmaConstants.UPSTREAM_COMMIT);
        System.out.println(TAG + " Initializing framework-level package hiding runtime in system_server");

        sHookState.set(HmaConstants.STATE_ENTRYPOINT_EXECUTED);
        sObservedReported.set(false);

        boolean configOk = false;
        if (configJson != null && !configJson.trim().isEmpty()) {
            configOk = sPolicyManager.applyConfigSnapshot(configJson);
        }

        if (!configOk) {
            configOk = sPolicyManager.loadConfig("/data/adb/modules/kaorioshelper/data/hma.json");
            if (!configOk) {
                configOk = sPolicyManager.loadConfig("/data/adb/modules/kaorioshelper/config/hma.json");
            }
        }

        System.out.println(TAG + " Configuration loaded: " + configOk
            + " (global hidden: " + sPolicyManager.getGlobalHiddenPackages().size() + ")");

        sHookState.set(HmaConstants.STATE_ART_HOOK_INIT);
        boolean pmsHookOk = PackageManagerHook.install(sPolicyManager, sInstance);
        if (pmsHookOk) {
            SettingsProviderHook.install(sPolicyManager, sInstance);
            System.out.println(TAG + " Framework-level package hooks successfully installed; current state: " + getHookStateName());
            reportNativeState(sHookState.get(), "HMA hooks installed synchronously: " + getHookStateName());
        } else if (!Boolean.getBoolean("kaorios.hma.disable_deferred_bootstrap")) {
            startDeferredBootstrap();
        } else {
            System.err.println(TAG + " Framework hooks could not be installed in current context");
        }
    }

    /**
     * During Zygisk postServerSpecialize, SystemServer.startBootstrapServices() has not yet
     * registered PackageManagerService or SettingsProvider. Poll non-blockingly on a daemon
     * thread until services become available and attach hooks immediately.
     */
    private static void startDeferredBootstrap() {
        if (!sBootstrapRunning.compareAndSet(false, true)) {
            return;
        }
        Thread bootstrap = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    for (int attempt = 0; attempt < 120; attempt++) {
                        try {
                            Thread.sleep(attempt < 20 ? 100L : 500L);
                        } catch (InterruptedException ie) {
                            Thread.currentThread().interrupt();
                            return;
                        }
                        if (!PackageManagerHook.isPackageServiceReady()) {
                            continue;
                        }
                        if (PackageManagerHook.install(sPolicyManager, sInstance)) {
                            SettingsProviderHook.install(sPolicyManager, sInstance);
                            int st = sHookState.get();
                            System.out.println(TAG + " Deferred system_server bootstrap installed HMA hooks: " + getHookStateName());
                            reportNativeState(st, "HMA deferred system_server hooks active: " + getHookStateName());
                            // Attempt late SettingsProvider attachment once ActivityThread provider map populates
                            for (int spAttempt = 0; spAttempt < 20; spAttempt++) {
                                if (SettingsProviderHook.install(sPolicyManager, sInstance)) {
                                    break;
                                }
                                try {
                                    Thread.sleep(250L);
                                } catch (InterruptedException ie) {
                                    Thread.currentThread().interrupt();
                                    return;
                                }
                            }
                            return;
                        }
                    }
                    System.err.println(TAG + " Deferred system_server bootstrap timed out waiting for PackageManagerService");
                    reportNativeState(HmaConstants.STATE_ART_HOOK_INIT, "Timed out waiting for PackageManagerService registration");
                } finally {
                    sBootstrapRunning.set(false);
                }
            }
        }, "Kaorios-HmaBootstrap");
        bootstrap.setDaemon(true);
        bootstrap.start();
    }

    public static void main() {
        main(new String[0]);
    }

    public static void main(String[] args) {
        init(null);
    }

    public void setHookState(int state) {
        sHookState.set(state);
    }

    public void notifyCallerPolicyApplied() {
        int current = sHookState.get();
        if (current == HmaConstants.STATE_PMS_HOOK_INSTALLED || current == HmaConstants.STATE_APPSFILTER_HOOKED) {
            sHookState.compareAndSet(current, HmaConstants.STATE_CALLER_POLICY_APPLIED);
        }
    }

    public void notifyResponseFiltered() {
        sHookState.set(HmaConstants.STATE_RESPONSE_FILTERED);
        if (sObservedReported.compareAndSet(false, true)) {
            reportNativeState(HmaConstants.STATE_RESPONSE_FILTERED, "HMA package/settings response filtered at runtime");
        }
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

    public static boolean shouldHide(int callingUid, String[] callingPkgs, String targetPkg) {
        return sPolicyManager.shouldHide(callingUid, callingPkgs, targetPkg);
    }

    public static HmaPolicyManager getPolicyManager() {
        return sPolicyManager;
    }
}
