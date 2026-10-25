/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * Pinned Upstream Commit: frknkrc44/HMA-OSS @ 1fe86ce2b4a6f21197e6e92d15e054d609b7d77c
 */

package org.frknkrc44.hma_oss.zygote;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicLong;

public class HmaPolicyManager {
    private static final String TAG = "[Kaorios/HmaPolicy]";

    // Blacklist scope: caller package -> set of hidden target packages
    private final Map<String, Set<String>> scopeMap = new HashMap<String, Set<String>>();
    // Whitelist scope: caller package -> set of visible allowed packages
    private final Map<String, Set<String>> whitelistMap = new HashMap<String, Set<String>>();
    // Installer spoofing map: target package -> spoofed installer (e.g. com.android.vending)
    private final Map<String, String> installerSpoofMap = new HashMap<String, String>();
    // Global hidden packages (root managers, injection frameworks, tamper detectors)
    private final Set<String> globalHiddenPackages = new HashSet<String>();
    private final AtomicLong filterCount = new AtomicLong(0);
    private boolean configLoaded = false;

    public HmaPolicyManager() {
        initDefaultGlobalPackages();
    }

    private void initDefaultGlobalPackages() {
        globalHiddenPackages.add("com.topjohnwu.magisk");
        globalHiddenPackages.add("io.github.vvb2060.magisk");
        globalHiddenPackages.add("me.weishu.kernelsu");
        globalHiddenPackages.add("org.apatch");
        globalHiddenPackages.add("org.lsposed.manager");
        globalHiddenPackages.add("com.cofface.ivroot");
        globalHiddenPackages.add("eu.chainfire.supersu");
        globalHiddenPackages.add("com.koushikdutta.superuser");
        globalHiddenPackages.add("de.robv.android.xposed.installer");
    }

    /**
     * Applies an in-memory JSON configuration snapshot passed directly via JNI.
     * This avoids DAC permission denials in system_server (UID 1000) when
     * reading 0700 root:root module storage directories.
     */
    public synchronized boolean applyConfigSnapshot(String json) {
        if (json == null || json.trim().isEmpty()) {
            return false;
        }

        try {
            parseConfigJson(json);
            configLoaded = true;
            System.out.println(TAG + " In-memory configuration snapshot applied successfully: "
                + scopeMap.size() + " blacklists, " + whitelistMap.size() + " whitelists, "
                + installerSpoofMap.size() + " installer spoofs");
            return true;
        } catch (Throwable t) {
            System.err.println(TAG + " Failed to parse config snapshot: " + t.getMessage());
            return false;
        }
    }

    public synchronized boolean loadConfig(String configPath) {
        File file = new File(configPath);
        if (!file.exists() || !file.canRead()) {
            return false;
        }

        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line).append("\n");
            }
            return applyConfigSnapshot(sb.toString());
        } catch (Throwable t) {
            System.err.println(TAG + " Failed to load config from " + configPath + ": " + t.getMessage());
            return false;
        }
    }

    private void parseConfigJson(String json) {
        scopeMap.clear();
        whitelistMap.clear();
        installerSpoofMap.clear();
        globalHiddenPackages.clear();
        initDefaultGlobalPackages();

        // Parse section markers and entries from JSON
        String section = "root";
        String currentCaller = null;
        boolean inArray = false;

        for (String rawLine : json.split("\n")) {
            String line = rawLine.trim();
            if (line.isEmpty() || line.startsWith("//")) continue;

            if (line.contains("\"scope\"")) {
                section = "scope";
                currentCaller = null;
                continue;
            } else if (line.contains("\"whitelist\"")) {
                section = "whitelist";
                currentCaller = null;
                continue;
            } else if (line.contains("\"installer_spoofs\"") || line.contains("\"installers\"")) {
                section = "installers";
                currentCaller = null;
                continue;
            } else if (line.contains("\"global_hidden\"") || line.contains("\"globalHidden\"")) {
                section = "global_hidden";
                currentCaller = null;
                continue;
            }

            if ("scope".equals(section) || "whitelist".equals(section)) {
                if (line.startsWith("}") && !line.contains("{")) {
                    currentCaller = null;
                    inArray = false;
                }
                int q1 = line.indexOf('"');
                if (q1 >= 0) {
                    int q2 = line.indexOf('"', q1 + 1);
                    if (q2 > q1) {
                        String token = line.substring(q1 + 1, q2);
                        if (line.contains(":") && (line.contains("{") || line.contains("["))) {
                            currentCaller = token;
                            if ("whitelist".equals(section)) {
                                whitelistMap.put(currentCaller, new HashSet<String>());
                            } else {
                                scopeMap.put(currentCaller, new HashSet<String>());
                            }
                            if (line.contains("[")) inArray = true;
                        } else if (currentCaller != null) {
                            if ("whitelist".equals(section)) {
                                Set<String> set = whitelistMap.get(currentCaller);
                                if (set != null) set.add(token);
                            } else {
                                Set<String> set = scopeMap.get(currentCaller);
                                if (set != null) set.add(token);
                            }
                        }
                    }
                }
            } else if ("installers".equals(section)) {
                // Format: "pkg.to.spoof": "com.android.vending"
                int q1 = line.indexOf('"');
                if (q1 >= 0) {
                    int q2 = line.indexOf('"', q1 + 1);
                    int q3 = line.indexOf('"', q2 + 1);
                    if (q3 > q2) {
                        int q4 = line.indexOf('"', q3 + 1);
                        if (q4 > q3) {
                            String targetPkg = line.substring(q1 + 1, q2);
                            String installer = line.substring(q3 + 1, q4);
                            installerSpoofMap.put(targetPkg, installer);
                        }
                    }
                }
            } else if ("global_hidden".equals(section)) {
                int q1 = line.indexOf('"');
                if (q1 >= 0) {
                    int q2 = line.indexOf('"', q1 + 1);
                    if (q2 > q1) {
                        String pkg = line.substring(q1 + 1, q2);
                        if (!pkg.equals("global_hidden") && !pkg.equals("globalHidden")) {
                            globalHiddenPackages.add(pkg);
                        }
                    }
                }
            }
        }
    }

    /**
     * Determines whether targetPackage should be hidden from caller.
     * System packages (UID 1000), root callers (UID 0), and internal system services
     * are NEVER filtered to prevent system instability or bootloops.
     */
    public boolean shouldHide(int callingUid, String[] callingPackages, String targetPackage) {
        // Enforce system package and root exemptions
        if (callingUid == HmaConstants.UID_ROOT || callingUid == HmaConstants.UID_SYSTEM) {
            return false;
        }
        if (callingUid < HmaConstants.FIRST_APPLICATION_UID) {
            return false;
        }
        if (targetPackage == null || targetPackage.isEmpty()) {
            return false;
        }

        // Check global blacklist (root managers, detectors)
        if (globalHiddenPackages.contains(targetPackage)) {
            filterCount.incrementAndGet();
            return true;
        }

        // Check caller-specific scoped policies
        if (callingPackages != null) {
            for (String caller : callingPackages) {
                // 1. Whitelist mode: if caller has a whitelist, only packages in whitelist are visible
                Set<String> allowed = whitelistMap.get(caller);
                if (allowed != null && !allowed.isEmpty()) {
                    if (!allowed.contains(targetPackage)) {
                        filterCount.incrementAndGet();
                        return true;
                    }
                }

                // 2. Blacklist mode: if caller has a blacklist, packages in blacklist are hidden
                Set<String> hidden = scopeMap.get(caller);
                if (hidden != null && hidden.contains(targetPackage)) {
                    filterCount.incrementAndGet();
                    return true;
                }
            }
        }

        return false;
    }

    /**
     * Returns spoofed installer package name if configured, or null if no spoofing is requested.
     */
    public String getSpoofedInstaller(String packageName) {
        if (packageName == null) return null;
        return installerSpoofMap.get(packageName);
    }

    public boolean isInstallerSpoofConfigured(String packageName) {
        return packageName != null && installerSpoofMap.containsKey(packageName);
    }

    public long getFilterCount() {
        return filterCount.get();
    }

    public boolean isConfigLoaded() {
        return configLoaded;
    }

    public Set<String> getGlobalHiddenPackages() {
        return Collections.unmodifiableSet(globalHiddenPackages);
    }

    public Map<String, Set<String>> getScopeMap() {
        return Collections.unmodifiableMap(scopeMap);
    }

    public Map<String, Set<String>> getWhitelistMap() {
        return Collections.unmodifiableMap(whitelistMap);
    }
}
