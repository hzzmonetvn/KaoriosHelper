/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * Pinned Upstream Commit: frknkrc44/HMA-OSS @ 1fe86ce2b4a6f21197e6e92d15e054d609b7d77c
 */

package org.frknkrc44.hma_oss.zygote;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

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
        boolean appsFilterHooked = false;
        boolean pmsHooked = false;

        try {
            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Method getPmsMethod = activityThreadClass.getDeclaredMethod("getPackageManager");
            getPmsMethod.setAccessible(true);
            Object realPms = getPmsMethod.invoke(null);

            Class<?> smClass = Class.forName("android.os.ServiceManager");
            Method getServiceMethod = smClass.getMethod("getService", String.class);
            Object binderPms = getServiceMethod.invoke(null, "package");

            Object targetPms = (realPms != null) ? realPms : binderPms;
            if (targetPms == null) {
                System.err.println(TAG + " Unable to locate PackageManager instance in system_server");
                return false;
            }

            // 1. Framework-level AppsFilter / AppsFilterImpl Hooking
            appsFilterHooked = installAppsFilterHook(targetPms, policyManager, parentEntry);
            if (appsFilterHooked) {
                parentEntry.setHookState(HmaConstants.STATE_APPSFILTER_HOOKED);
                System.out.println(TAG + " Successfully intercepted AppsFilter / AppsFilterImpl at framework boundary");
            }

            // 2. IPackageManager Dynamic Proxy Hooking
            Class<?> ipmsClass = Class.forName("android.content.pm.IPackageManager");
            PackageManagerHook handler = new PackageManagerHook(targetPms, policyManager, parentEntry);
            Object proxyPms = Proxy.newProxyInstance(
                targetPms.getClass().getClassLoader(),
                new Class<?>[]{ipmsClass},
                handler
            );

            // Replace ActivityThread.sPackageManager
            Field sPmsField = activityThreadClass.getDeclaredField("sPackageManager");
            sPmsField.setAccessible(true);
            sPmsField.set(null, proxyPms);

            // Replace ServiceManager sCache entry for "package"
            try {
                Field sCacheField = smClass.getDeclaredField("sCache");
                sCacheField.setAccessible(true);
                @SuppressWarnings("unchecked")
                Map<String, Object> sCache = (Map<String, Object>) sCacheField.get(null);
                if (sCache != null) {
                    // Wrap binder in a forwarding wrapper that returns proxyPms on queryLocalInterface
                    sCache.put("package", createBinderWrapper(binderPms != null ? binderPms : targetPms, proxyPms, ipmsClass));
                    System.out.println(TAG + " Updated ServiceManager.sCache with IPackageManager proxy wrapper");
                }
            } catch (Throwable t) {
                System.out.println(TAG + " Notice: ServiceManager sCache wrapping non-fatal: " + t.getMessage());
            }

            pmsHooked = true;
            if (!appsFilterHooked) {
                parentEntry.setHookState(HmaConstants.STATE_PMS_HOOK_INSTALLED);
            }
            System.out.println(TAG + " Successfully installed framework-level IPackageManager hook");
            return true;
        } catch (Throwable t) {
            System.err.println(TAG + " Framework hook installation failed: " + t.getMessage());
            return false;
        }
    }

    private static boolean installAppsFilterHook(Object targetPms, final HmaPolicyManager policyManager, final ZygoteEntry parentEntry) {
        try {
            // Find mAppsFilter field in targetPms or its enclosing classes
            Field appsFilterField = null;
            Class<?> curr = targetPms.getClass();
            while (curr != null && curr != Object.class) {
                try {
                    appsFilterField = curr.getDeclaredField("mAppsFilter");
                    break;
                } catch (NoSuchFieldException e) {
                    curr = curr.getSuperclass();
                }
            }

            if (appsFilterField == null) {
                return false;
            }

            appsFilterField.setAccessible(true);
            final Object origAppsFilter = appsFilterField.get(targetPms);
            if (origAppsFilter == null) {
                return false;
            }

            // Check if AppsFilter implements AppsFilterSnapshot interface (Android 13+)
            Class<?>[] interfaces = origAppsFilter.getClass().getInterfaces();
            Class<?> snapshotInterface = null;
            for (Class<?> iface : interfaces) {
                if (iface.getName().contains("AppsFilterSnapshot") || iface.getName().contains("AppsFilter")) {
                    snapshotInterface = iface;
                    break;
                }
            }

            if (snapshotInterface != null) {
                Object proxyAppsFilter = Proxy.newProxyInstance(
                    origAppsFilter.getClass().getClassLoader(),
                    new Class<?>[]{snapshotInterface},
                    new InvocationHandler() {
                        @Override
                        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
                            if ("shouldFilterApplication".equals(method.getName()) && args != null) {
                                int callingUid = extractCallingUidFromArgs(args);
                                String targetPkg = extractPackageNameFromSetting(args);

                                if (callingUid > 0 && targetPkg != null) {
                                    parentEntry.notifyCallerPolicyApplied();
                                    if (policyManager.shouldHide(callingUid, null, targetPkg)) {
                                        parentEntry.notifyResponseFiltered();
                                        return Boolean.TRUE; // true = filter (hide) from caller
                                    }
                                }
                            }
                            return method.invoke(origAppsFilter, args);
                        }
                    }
                );
                appsFilterField.set(targetPms, proxyAppsFilter);
                return true;
            }
        } catch (Throwable t) {
            System.out.println(TAG + " AppsFilter dynamic proxy hook non-fatal: " + t.getMessage());
        }
        return false;
    }

    private static Object createBinderWrapper(final Object originalBinder, final Object proxyInterface, final Class<?> ipmsClass) {
        try {
            Class<?> ibinderClass = Class.forName("android.os.IBinder");
            return Proxy.newProxyInstance(
                originalBinder.getClass().getClassLoader(),
                new Class<?>[]{ibinderClass},
                new InvocationHandler() {
                    @Override
                    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
                        String name = method.getName();
                        if ("queryLocalInterface".equals(name) && args != null && args.length >= 1) {
                            String desc = (String) args[0];
                            if (desc != null && desc.equals("android.content.pm.IPackageManager")) {
                                return proxyInterface;
                            }
                        }
                        return method.invoke(originalBinder, args);
                    }
                }
            );
        } catch (Throwable t) {
            return originalBinder;
        }
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        String methodName = method.getName();
        int callingUid = getCallingUid();

        // 1. Single Package Queries: getPackageInfo & getApplicationInfo
        if ("getPackageInfo".equals(methodName) || "getApplicationInfo".equals(methodName)) {
            if (args != null && args.length >= 1 && args[0] instanceof String) {
                String targetPkg = (String) args[0];
                String[] callingPkgs = getPackagesForUid(callingUid);

                parentEntry.notifyCallerPolicyApplied();

                if (policyManager.shouldHide(callingUid, callingPkgs, targetPkg)) {
                    parentEntry.notifyResponseFiltered();
                    // Server-side IPackageManager IPC returns null when not found
                    return null;
                }
            }
        }

        // 2. Collection Queries: getInstalledPackages & getInstalledApplications
        if ("getInstalledPackages".equals(methodName) || "getInstalledApplications".equals(methodName)) {
            String[] callingPkgs = getPackagesForUid(callingUid);
            parentEntry.notifyCallerPolicyApplied();

            Object result = method.invoke(originalPms, args);
            return filterCollectionResult(result, callingUid, callingPkgs);
        }

        // 3. Intent Resolution Vectors
        if (methodName.startsWith("queryIntent") || methodName.startsWith("queryPersistent")) {
            String[] callingPkgs = getPackagesForUid(callingUid);
            parentEntry.notifyCallerPolicyApplied();

            Object result = method.invoke(originalPms, args);
            return filterCollectionResult(result, callingUid, callingPkgs);
        }

        if ("resolveIntent".equals(methodName) || "resolveService".equals(methodName)) {
            String[] callingPkgs = getPackagesForUid(callingUid);
            parentEntry.notifyCallerPolicyApplied();

            Object result = method.invoke(originalPms, args);
            if (result != null) {
                String pkg = extractPackageName(result);
                if (pkg != null && policyManager.shouldHide(callingUid, callingPkgs, pkg)) {
                    parentEntry.notifyResponseFiltered();
                    return null;
                }
            }
            return result;
        }

        // 4. Installer Spoofing: getInstallerPackageName & getInstallSourceInfo
        if ("getInstallerPackageName".equals(methodName) && args != null && args.length >= 1 && args[0] instanceof String) {
            String targetPkg = (String) args[0];
            String spoofed = policyManager.getSpoofedInstaller(targetPkg);
            if (spoofed != null) {
                parentEntry.notifyResponseFiltered();
                return spoofed;
            }
        }

        // 5. Packages for UID Query
        if ("getPackagesForUid".equals(methodName) && args != null && args.length >= 1 && args[0] instanceof Integer) {
            int queryUid = (Integer) args[0];
            Object result = method.invoke(originalPms, args);
            if (result instanceof String[]) {
                String[] pkgs = (String[]) result;
                List<String> visible = new ArrayList<String>();
                boolean modified = false;
                for (String p : pkgs) {
                    if (policyManager.shouldHide(callingUid, null, p)) {
                        modified = true;
                        parentEntry.notifyResponseFiltered();
                    } else {
                        visible.add(p);
                    }
                }
                return modified ? visible.toArray(new String[0]) : result;
            }
            return result;
        }

        return method.invoke(originalPms, args);
    }

    private Object filterCollectionResult(Object result, int callingUid, String[] callingPkgs) {
        if (result == null) return null;

        // Check for ParceledListSlice
        Class<?> clazz = result.getClass();
        if ("android.content.pm.ParceledListSlice".equals(clazz.getName())) {
            try {
                Method getListMethod = clazz.getMethod("getList");
                @SuppressWarnings("unchecked")
                List<Object> originalList = (List<Object>) getListMethod.invoke(result);
                if (originalList == null) return result;

                List<Object> filteredList = new ArrayList<Object>();
                boolean modified = false;

                for (Object item : originalList) {
                    String pkg = extractPackageName(item);
                    if (pkg != null && policyManager.shouldHide(callingUid, callingPkgs, pkg)) {
                        modified = true;
                        parentEntry.notifyResponseFiltered();
                    } else {
                        filteredList.add(item);
                    }
                }

                if (!modified) return result;

                // Re-package into new ParceledListSlice
                Constructor<?> ctor = clazz.getConstructor(List.class);
                return ctor.newInstance(filteredList);
            } catch (Throwable t) {
                System.err.println(TAG + " ParceledListSlice filtering fallback: " + t.getMessage());
                return result;
            }
        } else if (result instanceof List) {
            @SuppressWarnings("unchecked")
            List<Object> list = (List<Object>) result;
            List<Object> filtered = new ArrayList<Object>();
            boolean modified = false;

            for (Object item : list) {
                String pkg = extractPackageName(item);
                if (pkg != null && policyManager.shouldHide(callingUid, callingPkgs, pkg)) {
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

    private static int extractCallingUidFromArgs(Object[] args) {
        for (Object arg : args) {
            if (arg instanceof Integer) {
                int val = (Integer) arg;
                if (val >= 1000) return val;
            }
        }
        return -1;
    }

    private static String extractPackageNameFromSetting(Object[] args) {
        for (Object arg : args) {
            if (arg != null) {
                try {
                    Method getPkgName = arg.getClass().getMethod("getPackageName");
                    return (String) getPkgName.invoke(arg);
                } catch (Throwable ignored) {}
                try {
                    Method getName = arg.getClass().getMethod("getName");
                    return (String) getName.invoke(arg);
                } catch (Throwable ignored) {}
            }
        }
        return null;
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
        } catch (Throwable ignored) {}

        // For ResolveInfo: check activityInfo, serviceInfo, providerInfo
        try {
            Field actField = info.getClass().getField("activityInfo");
            Object act = actField.get(info);
            if (act != null) {
                Field pField = act.getClass().getField("packageName");
                return (String) pField.get(act);
            }
        } catch (Throwable ignored) {}

        try {
            Field srvField = info.getClass().getField("serviceInfo");
            Object srv = srvField.get(info);
            if (srv != null) {
                Field pField = srv.getClass().getField("packageName");
                return (String) pField.get(srv);
            }
        } catch (Throwable ignored) {}

        try {
            Field provField = info.getClass().getField("providerInfo");
            Object prov = provField.get(info);
            if (prov != null) {
                Field pField = prov.getClass().getField("packageName");
                return (String) pField.get(prov);
            }
        } catch (Throwable ignored) {}

        return null;
    }
}
