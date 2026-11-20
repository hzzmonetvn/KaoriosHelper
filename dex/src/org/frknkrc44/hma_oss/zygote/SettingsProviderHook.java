/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * Pinned Upstream Commit: frknkrc44/HMA-OSS @ 1fe86ce2b4a6f21197e6e92d15e054d609b7d77c
 */

package org.frknkrc44.hma_oss.zygote;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Map;

/**
 * In-process SettingsProvider IContentProvider wrapper inside system_server.
 * Spoofs developer options / ADB enabled queries ("development_settings_enabled", "adb_enabled")
 * to "0" for scoped app callers while preserving real values for Settings UI, shell, and system UIDs.
 */
public final class SettingsProviderHook implements InvocationHandler {
    private static final String TAG = "[Kaorios/SettingsHook]";

    private final Object originalProvider;
    private final HmaPolicyManager policyManager;
    private final ZygoteEntry parentEntry;

    public SettingsProviderHook(Object originalProvider, HmaPolicyManager policyManager, ZygoteEntry parentEntry) {
        this.originalProvider = originalProvider;
        this.policyManager = policyManager;
        this.parentEntry = parentEntry;
    }

    /**
     * Pure policy evaluation helper for SettingsProvider call() interception.
     * Returns "0" when the query must be spoofed, or null when untouched.
     */
    public static String evaluateSettingCallOverride(HmaPolicyManager policyManager,
                                                     int callingUid,
                                                     String[] callingPackages,
                                                     String callMethod,
                                                     String settingKey) {
        if (policyManager == null || callMethod == null || settingKey == null) {
            return null;
        }
        return policyManager.getSpoofedSettingValue(callingUid, callingPackages, callMethod, settingKey);
    }

    public static boolean install(HmaPolicyManager policyManager, ZygoteEntry parentEntry) {
        try {
            Class<?> atClass = Class.forName("android.app.ActivityThread");
            Method currentAtMethod = atClass.getDeclaredMethod("currentActivityThread");
            currentAtMethod.setAccessible(true);
            Object activityThread = currentAtMethod.invoke(null);
            if (activityThread == null) {
                return false;
            }

            Class<?> iContentProviderClass = Class.forName("android.content.IContentProvider");
            boolean hooked = false;

            for (String mapFieldName : new String[]{"mProviderMap", "mLocalProvidersByName"}) {
                try {
                    Field mapField = atClass.getDeclaredField(mapFieldName);
                    mapField.setAccessible(true);
                    Object mapObj = mapField.get(activityThread);
                    if (!(mapObj instanceof Map)) continue;

                    @SuppressWarnings("unchecked")
                    Map<Object, Object> providerMap = (Map<Object, Object>) mapObj;
                    for (Map.Entry<Object, Object> entry : providerMap.entrySet()) {
                        Object key = entry.getKey();
                        Object record = entry.getValue();
                        if (key == null || record == null) continue;
                        if (!key.toString().contains("settings")) continue;

                        Field mProviderField = record.getClass().getDeclaredField("mProvider");
                        mProviderField.setAccessible(true);
                        Object origProvider = mProviderField.get(record);
                        if (origProvider == null) continue;
                        if (Proxy.isProxyClass(origProvider.getClass())
                                && Proxy.getInvocationHandler(origProvider) instanceof SettingsProviderHook) {
                            hooked = true;
                            continue;
                        }

                        Object proxy = Proxy.newProxyInstance(
                            iContentProviderClass.getClassLoader() != null
                                ? iContentProviderClass.getClassLoader()
                                : origProvider.getClass().getClassLoader(),
                            new Class<?>[]{iContentProviderClass},
                            new SettingsProviderHook(origProvider, policyManager, parentEntry)
                        );
                        mProviderField.set(record, proxy);
                        hooked = true;
                    }
                } catch (Throwable ignored) {}
            }

            if (hooked) {
                System.out.println(TAG + " Installed SettingsProvider IContentProvider hook in system_server");
            }
            return hooked;
        } catch (Throwable t) {
            return false;
        }
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        if ("call".equals(method.getName()) && args != null && args.length >= 3) {
            try {
                String callMethod = null;
                String settingKey = null;
                // Android 12+ IContentProvider.call(AttributionSource, String authority, String method, String arg, Bundle extras)
                // Older signatures: call(String callingPkg, String featureId, String authority, String method, String arg, Bundle extras)
                int stringCount = 0;
                String prev = null;
                for (Object a : args) {
                    if (a instanceof String) {
                        String s = (String) a;
                        if ("GET_global".equals(s) || "GET_secure".equals(s)) {
                            callMethod = s;
                        } else if (callMethod != null && settingKey == null) {
                            settingKey = s;
                            break;
                        }
                        prev = s;
                        stringCount++;
                    }
                }
                if (callMethod != null && settingKey != null) {
                    int callingUid = resolveCallingUid(args);
                    String[] callingPkgs = resolveCallingPackages(callingUid, args);
                    String override = evaluateSettingCallOverride(policyManager, callingUid, callingPkgs, callMethod, settingKey);
                    if (override != null) {
                        Object bundle = createSingleValueBundle(override);
                        if (bundle != null) {
                            parentEntry.notifyResponseFiltered();
                            return bundle;
                        }
                    }
                }
            } catch (Throwable ignored) {}
        }

        try {
            return method.invoke(originalProvider, args);
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw e.getTargetException();
        }
    }

    private static int resolveCallingUid(Object[] args) {
        if (args != null && args.length >= 1 && args[0] != null) {
            try {
                Method getUid = args[0].getClass().getMethod("getUid");
                Object v = getUid.invoke(args[0]);
                if (v instanceof Integer) {
                    int uid = ((Integer) v).intValue();
                    if (uid > 0) return uid;
                }
            } catch (Throwable ignored) {}
        }
        try {
            Class<?> binderClass = Class.forName("android.os.Binder");
            Method getCallingUid = binderClass.getDeclaredMethod("getCallingUid");
            return ((Integer) getCallingUid.invoke(null)).intValue();
        } catch (Throwable t) {
            return -1;
        }
    }

    private static String[] resolveCallingPackages(int callingUid, Object[] args) {
        if (args != null && args.length >= 1 && args[0] != null) {
            try {
                Method getPkg = args[0].getClass().getMethod("getPackageName");
                Object v = getPkg.invoke(args[0]);
                if (v instanceof String && !((String) v).isEmpty()) {
                    return new String[]{(String) v};
                }
            } catch (Throwable ignored) {}
        }
        if (callingUid >= HmaConstants.FIRST_APPLICATION_UID) {
            try {
                Class<?> atClass = Class.forName("android.app.ActivityThread");
                Method getPms = atClass.getDeclaredMethod("getPackageManager");
                getPms.setAccessible(true);
                Object pms = getPms.invoke(null);
                if (pms != null) {
                    Method getPkgs = pms.getClass().getMethod("getPackagesForUid", int.class);
                    Object res = getPkgs.invoke(pms, Integer.valueOf(callingUid));
                    if (res instanceof String[]) return (String[]) res;
                }
            } catch (Throwable ignored) {}
        }
        return null;
    }

    private static Object createSingleValueBundle(String value) {
        try {
            Class<?> bundleClass = Class.forName("android.os.Bundle");
            Object bundle = bundleClass.getConstructor(int.class).newInstance(Integer.valueOf(1));
            Method putString = bundleClass.getMethod("putString", String.class, String.class);
            putString.invoke(bundle, "value", value);
            return bundle;
        } catch (Throwable t) {
            return null;
        }
    }
}
