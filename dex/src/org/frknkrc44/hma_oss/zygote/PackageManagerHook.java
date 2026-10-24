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
import java.util.ArrayList;
import java.util.List;

public class PackageManagerHook implements InvocationHandler {
    private static final String TAG = "[Kaorios/PmsHook]";

    private final Object originalPms;
    private final HmaPolicyManager policyManager;
    private final ZygoteEntry parentEntry;

    public PackageManagerHook(Object originalPms, HmaPolicyManager policyManager, ZygoteEntry parentEntry) {
        this.originalPms = originalPms;
        this.policyManager = policyManager;
        this.parentEntry = parentEntry;
    }

    public static boolean install(HmaPolicyManager policyManager, ZygoteEntry parentEntry) {
        try {
            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Method getPmsMethod = activityThreadClass.getDeclaredMethod("getPackageManager");
            getPmsMethod.setAccessible(true);
            Object realPms = getPmsMethod.invoke(null);
            if (realPms == null) {
                System.err.println(TAG + " ActivityThread.getPackageManager() returned null");
                return false;
            }

            Class<?> ipmsClass = Class.forName("android.content.pm.IPackageManager");
            PackageManagerHook handler = new PackageManagerHook(realPms, policyManager, parentEntry);
            Object proxyPms = Proxy.newProxyInstance(
                realPms.getClass().getClassLoader(),
                new Class<?>[]{ipmsClass},
                handler
            );

            // Replace static sPackageManager in ActivityThread
            Field sPmsField = activityThreadClass.getDeclaredField("sPackageManager");
            sPmsField.setAccessible(true);
            sPmsField.set(null, proxyPms);

            System.out.println(TAG + " Successfully installed IPackageManager dynamic proxy hook");
            return true;
        } catch (Throwable t) {
            System.err.println(TAG + " PackageManager hook installation failed: " + t.getMessage());
            return false;
        }
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        String methodName = method.getName();

        // 1. Intercept getPackageInfo(String packageName, int/long flags, int userId)
        if ("getPackageInfo".equals(methodName) && args != null && args.length >= 1 && args[0] instanceof String) {
            String targetPkg = (String) args[0];
            int callingUid = getCallingUid();
            String[] callingPkgs = getPackagesForUid(callingUid);

            parentEntry.notifyCallerPolicyApplied();

            if (policyManager.shouldHide(callingUid, callingPkgs, targetPkg)) {
                parentEntry.notifyResponseFiltered();
                // Throw NameNotFoundException to simulate that target package does not exist
                Class<?> exClass = Class.forName("android.content.pm.PackageManager$NameNotFoundException");
                throw (Throwable) exClass.getConstructor(String.class).newInstance(targetPkg);
            }
        }

        // 2. Intercept getApplicationInfo(String packageName, int/long flags, int userId)
        if ("getApplicationInfo".equals(methodName) && args != null && args.length >= 1 && args[0] instanceof String) {
            String targetPkg = (String) args[0];
            int callingUid = getCallingUid();
            String[] callingPkgs = getPackagesForUid(callingUid);

            parentEntry.notifyCallerPolicyApplied();

            if (policyManager.shouldHide(callingUid, callingPkgs, targetPkg)) {
                parentEntry.notifyResponseFiltered();
                Class<?> exClass = Class.forName("android.content.pm.PackageManager$NameNotFoundException");
                throw (Throwable) exClass.getConstructor(String.class).newInstance(targetPkg);
            }
        }

        // 3. Intercept getInstalledPackages / getInstalledApplications
        if (("getInstalledPackages".equals(methodName) || "getInstalledApplications".equals(methodName))) {
            int callingUid = getCallingUid();
            String[] callingPkgs = getPackagesForUid(callingUid);

            parentEntry.notifyCallerPolicyApplied();

            Object result = method.invoke(originalPms, args);
            if (result instanceof List) {
                List<?> list = (List<?>) result;
                List<Object> filtered = new ArrayList<Object>();
                boolean modified = false;

                for (Object item : list) {
                    String pkgName = extractPackageName(item);
                    if (pkgName != null && policyManager.shouldHide(callingUid, callingPkgs, pkgName)) {
                        modified = true;
                        parentEntry.notifyResponseFiltered();
                    } else {
                        filtered.add(item);
                    }
                }
                return modified ? filtered : result;
            }
            return result;
        }

        return method.invoke(originalPms, args);
    }

    private int getCallingUid() {
        try {
            Class<?> binderClass = Class.forName("android.os.Binder");
            Method getCallingUidMethod = binderClass.getDeclaredMethod("getCallingUid");
            return (Integer) getCallingUidMethod.invoke(null);
        } catch (Throwable t) {
            return -1;
        }
    }

    private String[] getPackagesForUid(int uid) {
        if (uid <= 0) return null;
        try {
            Method getPkgsMethod = originalPms.getClass().getMethod("getPackagesForUid", int.class);
            return (String[]) getPkgsMethod.invoke(originalPms, uid);
        } catch (Throwable t) {
            return null;
        }
    }

    private String extractPackageName(Object info) {
        if (info == null) return null;
        try {
            Field pkgField = info.getClass().getField("packageName");
            return (String) pkgField.get(info);
        } catch (Throwable t) {
            return null;
        }
    }
}
