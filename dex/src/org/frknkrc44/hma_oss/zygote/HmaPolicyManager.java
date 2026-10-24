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

    private final Map<String, Set<String>> scopeMap = new HashMap<String, Set<String>>();
    private final Set<String> globalHiddenPackages = new HashSet<String>();
    private final AtomicLong filterCount = new AtomicLong(0);
    private boolean configLoaded = false;

    public HmaPolicyManager() {
        // Default global root and tamper detection packages
        globalHiddenPackages.add("com.topjohnwu.magisk");
        globalHiddenPackages.add("io.github.vvb2060.magisk");
        globalHiddenPackages.add("me.weishu.kernelsu");
        globalHiddenPackages.add("org.apatch");
        globalHiddenPackages.add("org.lsposed.manager");
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
            parseSimpleJson(sb.toString());
            configLoaded = true;
            return true;
        } catch (Throwable t) {
            System.err.println(TAG + " Failed to load config from " + configPath + ": " + t.getMessage());
            return false;
        }
    }

    private void parseSimpleJson(String json) {
        scopeMap.clear();
        boolean inScope = false;
        String currentCaller = null;

        for (String line : json.split("\n")) {
            String trimmed = line.trim();
            if (trimmed.contains("\"scope\"")) {
                inScope = true;
                continue;
            }
            if (inScope) {
                if (trimmed.startsWith("}") && !trimmed.contains("{")) {
                    currentCaller = null;
                }
                int q1 = trimmed.indexOf('"');
                if (q1 >= 0) {
                    int q2 = trimmed.indexOf('"', q1 + 1);
                    if (q2 > q1) {
                        String token = trimmed.substring(q1 + 1, q2);
                        if (token.contains(".")) {
                            if (trimmed.contains("{") || trimmed.contains(":")) {
                                currentCaller = token;
                                scopeMap.put(currentCaller, new HashSet<String>());
                            } else if (currentCaller != null) {
                                Set<String> list = scopeMap.get(currentCaller);
                                if (list != null) {
                                    list.add(token);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /**
     * Determines whether targetPackage should be hidden from caller.
     * System packages and root callers are NEVER filtered.
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

        // Check global blacklist
        if (globalHiddenPackages.contains(targetPackage)) {
            filterCount.incrementAndGet();
            return true;
        }

        // Check caller-specific scoped policies
        if (callingPackages != null) {
            for (String caller : callingPackages) {
                Set<String> hidden = scopeMap.get(caller);
                if (hidden != null && hidden.contains(targetPackage)) {
                    filterCount.incrementAndGet();
                    return true;
                }
            }
        }

        return false;
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
}
