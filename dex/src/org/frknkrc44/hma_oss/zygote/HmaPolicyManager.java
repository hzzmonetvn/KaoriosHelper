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
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicLong;

public class HmaPolicyManager {
    private static final String TAG = "[Kaorios/HmaPolicy]";
    private static final String DEFAULT_PLAY_INSTALLER = "com.android.vending";

    // Blacklist scope: caller package -> set of hidden target packages
    private final Map<String, Set<String>> scopeMap = new HashMap<String, Set<String>>();
    // Whitelist scope: caller package -> set of visible allowed packages
    private final Map<String, Set<String>> whitelistMap = new HashMap<String, Set<String>>();
    // Opposite exemptions in blacklist mode: caller package -> packages explicitly visible even if in template/global list
    private final Map<String, Set<String>> oppositeExemptMap = new HashMap<String, Set<String>>();
    // Custom per-caller Settings overrides from settingsTemplates + applySettingTemplates
    private final Map<String, Map<String, String>> callerSettingsOverrides = new HashMap<String, Map<String, String>>();
    // Installer spoofing map: target package -> spoofed installer (e.g. com.android.vending)
    private final Map<String, String> installerSpoofMap = new HashMap<String, String>();
    // Callers for which installation sources of visible apps should be sanitized to Play Store
    private final Set<String> hideInstallSourceCallers = new HashSet<String>();
    // Callers for which developer options / ADB settings queries should be hidden ("0")
    private final Set<String> hideDevSettingsCallers = new HashSet<String>();
    // Global hidden packages (root managers, injection frameworks, tamper detectors)
    private final Set<String> globalHiddenPackages = new HashSet<String>();
    private final AtomicLong filterCount = new AtomicLong(0);
    private boolean configLoaded = false;
    private boolean globalHideDeveloperOptions = false;

    public HmaPolicyManager() {
        initDefaultGlobalPackages();
    }

    private void initDefaultGlobalPackages() {
        globalHiddenPackages.add("com.topjohnwu.magisk");
        globalHiddenPackages.add("io.github.vvb2060.magisk");
        globalHiddenPackages.add("me.weishu.kernelsu");
        globalHiddenPackages.add("io.github.a13e300.ksu");
        globalHiddenPackages.add("org.apatch");
        globalHiddenPackages.add("me.bmax.apatch");
        globalHiddenPackages.add("org.lsposed.manager");
        globalHiddenPackages.add("com.cofface.ivroot");
        globalHiddenPackages.add("eu.chainfire.supersu");
        globalHiddenPackages.add("com.koushikdutta.superuser");
        globalHiddenPackages.add("de.robv.android.xposed.installer");
    }

    /**
     * Applies an in-memory JSON configuration snapshot passed directly via JNI.
     * Uses a full RFC 8259 parser so compact single-line and multiline JSON both work.
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
        Map<String, Object> root = SimpleJson.parseObject(json);

        Map<String, Set<String>> newScopeMap = new HashMap<String, Set<String>>();
        Map<String, Set<String>> newWhitelistMap = new HashMap<String, Set<String>>();
        Map<String, Set<String>> newOppositeExemptMap = new HashMap<String, Set<String>>();
        Map<String, Map<String, String>> newCallerSettings = new HashMap<String, Map<String, String>>();
        Map<String, String> newInstallerMap = new HashMap<String, String>();
        Set<String> newHideInstallCallers = new HashSet<String>();
        Set<String> newHideDevCallers = new HashSet<String>();
        Set<String> newGlobalHidden = new HashSet<String>();

        boolean keepDefaultGlobal = true;
        Object useDefaultObj = root.get("useDefaultGlobalHidden");
        if (useDefaultObj instanceof Boolean) {
            keepDefaultGlobal = ((Boolean) useDefaultObj).booleanValue();
        }
        if (keepDefaultGlobal) {
            newGlobalHidden.add("com.topjohnwu.magisk");
            newGlobalHidden.add("io.github.vvb2060.magisk");
            newGlobalHidden.add("me.weishu.kernelsu");
            newGlobalHidden.add("io.github.a13e300.ksu");
            newGlobalHidden.add("org.apatch");
            newGlobalHidden.add("me.bmax.apatch");
            newGlobalHidden.add("org.lsposed.manager");
            newGlobalHidden.add("com.cofface.ivroot");
            newGlobalHidden.add("eu.chainfire.supersu");
            newGlobalHidden.add("com.koushikdutta.superuser");
            newGlobalHidden.add("de.robv.android.xposed.installer");
        }

        boolean newGlobalHideDev = false;
        if (Boolean.TRUE.equals(root.get("hideDeveloperOptions")) || Boolean.TRUE.equals(root.get("hideAdbEnabled"))) {
            newGlobalHideDev = true;
        }

        // 1. Parse templates ({"TemplateName": {"appList": [...], "isWhitelist": bool}} or {"TemplateName": [...]})
        Map<String, Set<String>> templateLists = new HashMap<String, Set<String>>();
        Map<String, Boolean> templateWhitelistMode = new HashMap<String, Boolean>();
        Object templatesObj = root.get("templates");
        if (templatesObj instanceof Map) {
            @SuppressWarnings("unchecked")
            Map<String, Object> templates = (Map<String, Object>) templatesObj;
            for (Map.Entry<String, Object> entry : templates.entrySet()) {
                String tplName = entry.getKey();
                Object tplVal = entry.getValue();
                if (tplVal instanceof List) {
                    templateLists.put(tplName, extractStringSet((List<?>) tplVal));
                    templateWhitelistMode.put(tplName, Boolean.FALSE);
                } else if (tplVal instanceof Map) {
                    @SuppressWarnings("unchecked")
                    Map<String, Object> tplMap = (Map<String, Object>) tplVal;
                    Object appList = tplMap.get("appList");
                    if (appList instanceof List) {
                        templateLists.put(tplName, extractStringSet((List<?>) appList));
                    }
                    templateWhitelistMode.put(tplName, Boolean.valueOf(Boolean.TRUE.equals(tplMap.get("isWhitelist"))));
                }
            }
        }

        // 1b. Parse settingsTemplates ({"Tpl": {"settingsList": [...], "values": {k: v}}})
        Map<String, Map<String, String>> settingsTemplateMap = new HashMap<String, Map<String, String>>();
        Object settingsTplsObj = root.get("settingsTemplates");
        if (settingsTplsObj instanceof Map) {
            @SuppressWarnings("unchecked")
            Map<String, Object> stMap = (Map<String, Object>) settingsTplsObj;
            for (Map.Entry<String, Object> entry : stMap.entrySet()) {
                Map<String, String> resolved = new HashMap<String, String>();
                Object tplVal = entry.getValue();
                if (tplVal instanceof List) {
                    for (String k : extractStringSet((List<?>) tplVal)) {
                        resolved.put(k, "0");
                    }
                } else if (tplVal instanceof Map) {
                    @SuppressWarnings("unchecked")
                    Map<String, Object> tplMap = (Map<String, Object>) tplVal;
                    Object sList = tplMap.get("settingsList");
                    if (sList instanceof List) {
                        for (String k : extractStringSet((List<?>) sList)) {
                            resolved.put(k, "0");
                        }
                    }
                    Object valuesObj = tplMap.get("values");
                    if (valuesObj instanceof Map) {
                        @SuppressWarnings("unchecked")
                        Map<String, Object> valMap = (Map<String, Object>) valuesObj;
                        for (Map.Entry<String, Object> ve : valMap.entrySet()) {
                            String vStr = normalizeSettingValue(ve.getValue());
                            if (vStr != null) resolved.put(ve.getKey(), vStr);
                        }
                    }
                    for (Map.Entry<String, Object> kv : tplMap.entrySet()) {
                        String k = kv.getKey();
                        if ("settingsList".equals(k) || "values".equals(k) || "isWhitelist".equals(k)) continue;
                        String vStr = normalizeSettingValue(kv.getValue());
                        if (vStr != null) resolved.put(k, vStr);
                    }
                }
                if (!resolved.isEmpty()) {
                    settingsTemplateMap.put(entry.getKey(), resolved);
                }
            }
        }

        // 2. Parse global_hidden / globalHidden & ignoredPackagesForPresets
        for (String key : new String[]{"global_hidden", "globalHidden"}) {
            Object gh = root.get(key);
            if (gh instanceof List) {
                newGlobalHidden.addAll(extractStringSet((List<?>) gh));
            }
        }
        Set<String> ignoredPresetPkgs = new HashSet<String>();
        Object ignoredObj = root.get("ignoredPackagesForPresets");
        if (ignoredObj instanceof List) {
            ignoredPresetPkgs.addAll(extractStringSet((List<?>) ignoredObj));
        }

        // 3. Parse explicit whitelist section
        Object wlObj = root.get("whitelist");
        if (wlObj instanceof Map) {
            @SuppressWarnings("unchecked")
            Map<String, Object> wlMap = (Map<String, Object>) wlObj;
            for (Map.Entry<String, Object> entry : wlMap.entrySet()) {
                if (entry.getValue() instanceof List) {
                    newWhitelistMap.put(entry.getKey(), extractStringSet((List<?>) entry.getValue()));
                }
            }
        }

        // 4. Parse scope (supports both Kaorios array format and upstream HMA AppRule object format)
        Object scopeObj = root.get("scope");
        if (scopeObj instanceof Map) {
            @SuppressWarnings("unchecked")
            Map<String, Object> scope = (Map<String, Object>) scopeObj;
            for (Map.Entry<String, Object> entry : scope.entrySet()) {
                String caller = entry.getKey();
                Object ruleVal = entry.getValue();
                if (ruleVal instanceof List) {
                    Set<String> targets = extractStringSet((List<?>) ruleVal);
                    newScopeMap.put(caller, targets);
                } else if (ruleVal instanceof Map) {
                    @SuppressWarnings("unchecked")
                    Map<String, Object> rule = (Map<String, Object>) ruleVal;
                    boolean isWhitelist = Boolean.TRUE.equals(rule.get("useWhitelist"));
                    Set<String> targets = new HashSet<String>();

                    Object extraApps = rule.get("extraAppList");
                    if (extraApps instanceof List) {
                        targets.addAll(extractStringSet((List<?>) extraApps));
                    }
                    Object applyTpls = rule.get("applyTemplates");
                    if (applyTpls instanceof List) {
                        for (String tplName : extractStringSet((List<?>) applyTpls)) {
                            Set<String> members = templateLists.get(tplName);
                            if (members != null) {
                                targets.addAll(members);
                            }
                            if (Boolean.TRUE.equals(templateWhitelistMode.get(tplName))) {
                                isWhitelist = true;
                            }
                        }
                    }

                    Object applyPresets = rule.get("applyPresets");
                    if (applyPresets instanceof List) {
                        for (String presetName : extractStringSet((List<?>) applyPresets)) {
                            resolvePresetPackages(presetName, ignoredPresetPkgs, targets);
                        }
                    }

                    Set<String> opposites = new HashSet<String>();
                    Object oppositeApps = rule.get("extraOppositeAppList");
                    if (oppositeApps instanceof List) {
                        opposites.addAll(extractStringSet((List<?>) oppositeApps));
                    }
                    if (!opposites.isEmpty()) {
                        targets.removeAll(opposites);
                    }

                    if (Boolean.TRUE.equals(rule.get("hideInstallationSource"))
                            || Boolean.TRUE.equals(rule.get("hideSystemInstallationSource"))) {
                        newHideInstallCallers.add(caller);
                    }
                    if (Boolean.TRUE.equals(rule.get("hideDeveloperOptions"))) {
                        newHideDevCallers.add(caller);
                    }
                    Object settingPresets = rule.get("applySettingsPresets");
                    if (settingPresets instanceof List && !((List<?>) settingPresets).isEmpty()) {
                        newHideDevCallers.add(caller);
                    }
                    Object settingTpls = rule.get("applySettingTemplates");
                    if (settingTpls instanceof List && !((List<?>) settingTpls).isEmpty()) {
                        newHideDevCallers.add(caller);
                        for (String stName : extractStringSet((List<?>) settingTpls)) {
                            Map<String, String> tplValues = settingsTemplateMap.get(stName);
                            if (tplValues != null) {
                                Map<String, String> callerMap = newCallerSettings.get(caller);
                                if (callerMap == null) {
                                    callerMap = new HashMap<String, String>();
                                    newCallerSettings.put(caller, callerMap);
                                }
                                callerMap.putAll(tplValues);
                            }
                        }
                    }

                    if (isWhitelist) {
                        newWhitelistMap.put(caller, targets);
                        if (!opposites.isEmpty()) {
                            Set<String> existingScope = newScopeMap.get(caller);
                            if (existingScope == null) {
                                existingScope = new HashSet<String>();
                                newScopeMap.put(caller, existingScope);
                            }
                            existingScope.addAll(opposites);
                        }
                    } else {
                        newScopeMap.put(caller, targets);
                        if (!opposites.isEmpty()) {
                            newOppositeExemptMap.put(caller, opposites);
                        }
                    }
                }
            }
        }

        // 5. Parse installer spoofing maps & lists
        for (String key : new String[]{"installer_spoofs", "installers"}) {
            Object instObj = root.get(key);
            if (instObj instanceof Map) {
                @SuppressWarnings("unchecked")
                Map<String, Object> instMap = (Map<String, Object>) instObj;
                for (Map.Entry<String, Object> entry : instMap.entrySet()) {
                    if (entry.getValue() instanceof String) {
                        String val = ((String) entry.getValue()).trim();
                        if (!val.isEmpty()) {
                            newInstallerMap.put(entry.getKey(), val);
                        }
                    }
                }
            }
        }
        Object instSpoofList = root.get("installSourceSpoof");
        if (instSpoofList instanceof List) {
            for (String pkg : extractStringSet((List<?>) instSpoofList)) {
                if (!newInstallerMap.containsKey(pkg)) {
                    newInstallerMap.put(pkg, DEFAULT_PLAY_INSTALLER);
                }
            }
        }

        // 6. Parse settingsScope (explicit caller list for developer/ADB settings hiding)
        Object settingsScopeObj = root.get("settingsScope");
        if (settingsScopeObj instanceof List) {
            newHideDevCallers.addAll(extractStringSet((List<?>) settingsScopeObj));
        }

        scopeMap.clear();
        scopeMap.putAll(newScopeMap);
        whitelistMap.clear();
        whitelistMap.putAll(newWhitelistMap);
        oppositeExemptMap.clear();
        oppositeExemptMap.putAll(newOppositeExemptMap);
        callerSettingsOverrides.clear();
        callerSettingsOverrides.putAll(newCallerSettings);
        installerSpoofMap.clear();
        installerSpoofMap.putAll(newInstallerMap);
        hideInstallSourceCallers.clear();
        hideInstallSourceCallers.addAll(newHideInstallCallers);
        hideDevSettingsCallers.clear();
        hideDevSettingsCallers.addAll(newHideDevCallers);
        globalHiddenPackages.clear();
        globalHiddenPackages.addAll(newGlobalHidden);
        globalHideDeveloperOptions = newGlobalHideDev;
    }

    private static void resolvePresetPackages(String presetName, Set<String> ignored, Set<String> out) {
        if (presetName == null) return;
        String norm = presetName.trim().toLowerCase(java.util.Locale.ROOT);
        boolean addRoot = "root".equals(norm) || "magisk".equals(norm) || "kernelsu".equals(norm)
                || "apatch".equals(norm) || "default".equals(norm) || "all".equals(norm);
        boolean addXposed = "xposed".equals(norm) || "lsposed".equals(norm)
                || "default".equals(norm) || "all".equals(norm);
        boolean addHma = "hma".equals(norm) || "detectors".equals(norm) || "devtools".equals(norm)
                || "default".equals(norm) || "all".equals(norm);

        if (addRoot) {
            addPresetGroup(new String[]{
                "com.topjohnwu.magisk", "io.github.vvb2060.magisk", "io.github.huskydg.magisk",
                "me.weishu.kernelsu", "com.rifsxd.ksunext", "io.github.a13e300.ksu",
                "org.apatch", "me.bmax.apatch", "eu.chainfire.supersu", "com.koushikdutta.superuser"
            }, ignored, out);
        }
        if (addXposed) {
            addPresetGroup(new String[]{
                "org.lsposed.manager", "de.robv.android.xposed.installer",
                "org.meowcat.edxposed.manager", "rikka.sui"
            }, ignored, out);
        }
        if (addHma) {
            addPresetGroup(new String[]{
                "com.tsng.hidemyapplist", "org.frknkrc44.hma_oss",
                "rikka.shizuku.app", "moe.shizuku.privileged.api", "bin.mt.plus"
            }, ignored, out);
        }
    }

    private static void addPresetGroup(String[] candidates, Set<String> ignored, Set<String> out) {
        for (int i = 0; i < candidates.length; i++) {
            String pkg = candidates[i];
            if (ignored == null || !ignored.contains(pkg)) {
                out.add(pkg);
            }
        }
    }

    private static String normalizeSettingValue(Object val) {
        if (val == null) return null;
        if (val instanceof String) return (String) val;
        if (val instanceof Boolean) return ((Boolean) val).booleanValue() ? "1" : "0";
        if (val instanceof Number) return String.valueOf(val);
        return null;
    }

    private static Set<String> extractStringSet(List<?> items) {
        Set<String> out = new HashSet<String>();
        for (Object item : items) {
            if (item instanceof String) {
                String s = ((String) item).trim();
                if (!s.isEmpty()) {
                    out.add(s);
                }
            }
        }
        return out;
    }

    /**
     * Determines whether targetPackage should be hidden from caller.
     * System packages (UID 1000), root callers (UID 0), and internal system services
     * (< FIRST_APPLICATION_UID) are NEVER filtered to prevent system instability or bootloops.
     */
    public synchronized boolean shouldHide(int callingUid, String[] callingPackages, String targetPackage) {
        if (callingUid == HmaConstants.UID_ROOT || callingUid == HmaConstants.UID_SYSTEM) {
            return false;
        }
        if (callingUid < HmaConstants.FIRST_APPLICATION_UID) {
            return false;
        }
        if (targetPackage == null || targetPackage.isEmpty()) {
            return false;
        }

        // Caller must never hide its own package from itself
        if (callingPackages != null) {
            for (String caller : callingPackages) {
                if (targetPackage.equals(caller)) {
                    return false;
                }
            }
            // Blacklist-mode extraOppositeAppList explicitly exempts targetPackage for this caller
            for (String caller : callingPackages) {
                if (caller == null || caller.isEmpty()) continue;
                Set<String> exempt = oppositeExemptMap.get(caller);
                if (exempt != null && exempt.contains(targetPackage)) {
                    return false;
                }
            }
        }

        // Check global blacklist (root managers, detectors)
        if (globalHiddenPackages.contains(targetPackage)) {
            filterCount.incrementAndGet();
            return true;
        }

        // Check caller-specific scoped policies
        if (callingPackages != null) {
            for (String caller : callingPackages) {
                if (caller == null || caller.isEmpty()) continue;

                // 1. Whitelist mode: if caller has a non-empty whitelist, only packages in whitelist are visible
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
     * Returns spoofed installer package name if configured for targetPackage or caller rule.
     */
    public synchronized String getSpoofedInstaller(String packageName) {
        if (packageName == null) return null;
        return installerSpoofMap.get(packageName);
    }

    public synchronized String getSpoofedInstaller(int callingUid, String[] callingPackages, String targetPackage) {
        if (targetPackage == null) return null;
        if (callingUid >= 0 && callingUid < HmaConstants.FIRST_APPLICATION_UID) {
            return null;
        }
        String direct = installerSpoofMap.get(targetPackage);
        if (direct != null) {
            return direct;
        }
        if (callingPackages != null) {
            for (String caller : callingPackages) {
                if (hideInstallSourceCallers.contains(caller)) {
                    return DEFAULT_PLAY_INSTALLER;
                }
            }
        }
        return null;
    }

    public synchronized boolean isInstallerSpoofConfigured(String packageName) {
        return packageName != null && installerSpoofMap.containsKey(packageName);
    }

    /**
     * Evaluates whether Settings.Global / Settings.Secure developer/ADB flags should be spoofed to "0"
     * for the given caller. System/root/shell (< 10000) are strictly exempt.
     */
    public synchronized boolean shouldSpoofDeveloperSetting(int callingUid, String[] callingPackages, String settingKey) {
        if (callingUid < HmaConstants.FIRST_APPLICATION_UID || settingKey == null) {
            return false;
        }
        if (!"development_settings_enabled".equals(settingKey)
                && !"adb_enabled".equals(settingKey)
                && !"adb_wifi_enabled".equals(settingKey)
                && !"mock_location".equals(settingKey)) {
            return false;
        }
        if (globalHideDeveloperOptions) {
            filterCount.incrementAndGet();
            return true;
        }
        if (callingPackages != null) {
            for (String caller : callingPackages) {
                if (caller == null) continue;
                if (hideDevSettingsCallers.contains(caller)
                        || scopeMap.containsKey(caller)
                        || whitelistMap.containsKey(caller)) {
                    filterCount.incrementAndGet();
                    return true;
                }
            }
        }
        return false;
    }

    /**
     * Evaluates custom settingsTemplates overrides first, then falls back to developer/ADB
     * flags ("0") for scoped callers. System/root/shell (< 10000) are strictly exempt.
     */
    public synchronized String getSpoofedSettingValue(int callingUid, String[] callingPackages, String callMethod, String settingKey) {
        if (callingUid < HmaConstants.FIRST_APPLICATION_UID || callMethod == null || settingKey == null) {
            return null;
        }
        if (!"GET_global".equals(callMethod) && !"GET_secure".equals(callMethod)) {
            return null;
        }
        if (callingPackages != null) {
            for (String caller : callingPackages) {
                if (caller == null || caller.isEmpty()) continue;
                Map<String, String> custom = callerSettingsOverrides.get(caller);
                if (custom != null && custom.containsKey(settingKey)) {
                    filterCount.incrementAndGet();
                    return custom.get(settingKey);
                }
            }
        }
        if (shouldSpoofDeveloperSetting(callingUid, callingPackages, settingKey)) {
            return "0";
        }
        return null;
    }

    public long getFilterCount() {
        return filterCount.get();
    }

    public synchronized boolean isConfigLoaded() {
        return configLoaded;
    }

    public synchronized Set<String> getGlobalHiddenPackages() {
        return Collections.unmodifiableSet(new HashSet<String>(globalHiddenPackages));
    }

    public synchronized Map<String, Set<String>> getScopeMap() {
        return Collections.unmodifiableMap(new HashMap<String, Set<String>>(scopeMap));
    }

    public synchronized Map<String, Set<String>> getWhitelistMap() {
        return Collections.unmodifiableMap(new HashMap<String, Set<String>>(whitelistMap));
    }
}
