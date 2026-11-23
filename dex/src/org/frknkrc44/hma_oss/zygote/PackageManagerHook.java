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
    private static final ThreadLocal<Boolean> IN_HOOK = new ThreadLocal<Boolean>() {
        @Override
        protected Boolean initialValue() {
            return Boolean.FALSE;
        }
    };

    private final Object originalPms;
    private final Object uidLookupPms;
    private final HmaPolicyManager policyManager;
    private final ZygoteEntry parentEntry;
    private final boolean isComputerProxy;
    private static volatile Object sOuterPmsRef = null;

    public PackageManagerHook(Object originalPms, HmaPolicyManager policyManager, ZygoteEntry parentEntry) {
        this(originalPms, originalPms, policyManager, parentEntry, false);
    }

    private PackageManagerHook(Object delegateTarget,
                               Object uidLookupPms,
                               HmaPolicyManager policyManager,
                               ZygoteEntry parentEntry,
                               boolean isComputerProxy) {
        this.originalPms = delegateTarget;
        this.uidLookupPms = uidLookupPms;
        this.policyManager = policyManager;
        this.parentEntry = parentEntry;
        this.isComputerProxy = isComputerProxy;
    }

    /**
     * Non-blocking check used during deferred system_server bootstrap.
     */
    public static boolean isPackageServiceReady() {
        try {
            Class<?> smClass = Class.forName("android.os.ServiceManager");
            try {
                Method checkService = smClass.getMethod("checkService", String.class);
                Object binder = checkService.invoke(null, "package");
                if (binder != null) return true;
            } catch (NoSuchMethodException ignored) {
                Method getService = smClass.getMethod("getService", String.class);
                if (getService.invoke(null, "package") != null) return true;
            }
            Class<?> atClass = Class.forName("android.app.ActivityThread");
            Field sPmsField = atClass.getDeclaredField("sPackageManager");
            sPmsField.setAccessible(true);
            return sPmsField.get(null) != null;
        } catch (Throwable ignored) {
            return false;
        }
    }

    public static boolean install(HmaPolicyManager policyManager, ZygoteEntry parentEntry) {
        boolean appsFilterHooked = false;

        try {
            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Object realPms = null;
            try {
                Field sPmsFieldCheck = activityThreadClass.getDeclaredField("sPackageManager");
                sPmsFieldCheck.setAccessible(true);
                realPms = sPmsFieldCheck.get(null);
            } catch (Throwable ignored) {}
            if (realPms == null) {
                try {
                    Method getPmsMethod = activityThreadClass.getDeclaredMethod("getPackageManager");
                    getPmsMethod.setAccessible(true);
                    realPms = getPmsMethod.invoke(null);
                } catch (Throwable ignored) {}
            }

            Class<?> smClass = Class.forName("android.os.ServiceManager");
            Object binderPms = null;
            try {
                Method checkServiceMethod = smClass.getMethod("checkService", String.class);
                binderPms = checkServiceMethod.invoke(null, "package");
            } catch (NoSuchMethodException e) {
                Method getServiceMethod = smClass.getMethod("getService", String.class);
                binderPms = getServiceMethod.invoke(null, "package");
            }

            Object targetPms = (realPms != null) ? realPms : binderPms;
            if (targetPms == null) {
                System.err.println(TAG + " PackageManagerService not yet registered in system_server");
                return false;
            }

            // Avoid double-wrapping if already a Proxy handled by PackageManagerHook
            if (Proxy.isProxyClass(targetPms.getClass())
                    && Proxy.getInvocationHandler(targetPms) instanceof PackageManagerHook) {
                return true;
            }

            // 1. Framework-level AppsFilter / AppsFilterSnapshot & Computer Hooking (covers remote Binder callers)
            appsFilterHooked = installAppsFilterHook(targetPms, policyManager, parentEntry);
            boolean computerHooked = installComputerHook(targetPms, policyManager, parentEntry);
            if (appsFilterHooked || computerHooked) {
                parentEntry.setHookState(HmaConstants.STATE_APPSFILTER_HOOKED);
                System.out.println(TAG + " Successfully intercepted AppsFilter/Computer at framework boundary");
            }

            // 2. IPackageManager Dynamic Proxy Hooking
            Class<?> ipmsClass = Class.forName("android.content.pm.IPackageManager");
            PackageManagerHook handler = new PackageManagerHook(targetPms, policyManager, parentEntry);
            Object proxyPms = Proxy.newProxyInstance(
                ipmsClass.getClassLoader() != null ? ipmsClass.getClassLoader() : targetPms.getClass().getClassLoader(),
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
                    sCache.put("package", createBinderWrapper(binderPms != null ? binderPms : targetPms, proxyPms));
                    System.out.println(TAG + " Updated ServiceManager.sCache with IPackageManager proxy wrapper");
                }
            } catch (Throwable t) {
                System.out.println(TAG + " Notice: ServiceManager sCache wrapping non-fatal: " + t.getMessage());
            }

            if (!appsFilterHooked && !computerHooked) {
                parentEntry.setHookState(HmaConstants.STATE_PMS_HOOK_INSTALLED);
            }
            System.out.println(TAG + " Successfully installed framework-level IPackageManager hook");
            return true;
        } catch (Throwable t) {
            System.err.println(TAG + " Framework hook installation failed: " + t.getMessage());
            return false;
        }
    }

    private static Object resolveOuterPms(Object targetPms) {
        if (targetPms == null) return null;
        for (String fieldName : new String[]{"this$0", "mPM", "mService"}) {
            try {
                Field f = targetPms.getClass().getDeclaredField(fieldName);
                f.setAccessible(true);
                Object outer = f.get(targetPms);
                if (outer != null) return outer;
            } catch (Throwable ignored) {}
        }
        return null;
    }

    private static Field findFieldInHierarchy(Class<?> clazz, String name) {
        Class<?> curr = clazz;
        while (curr != null && curr != Object.class) {
            try {
                Field f = curr.getDeclaredField(name);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException e) {
                curr = curr.getSuperclass();
            }
        }
        return null;
    }

    private static List<Class<?>> collectAllInterfaces(Class<?> clazz) {
        List<Class<?>> allIfaces = new ArrayList<Class<?>>();
        Class<?> c = clazz;
        while (c != null && c != Object.class) {
            for (Class<?> iface : c.getInterfaces()) {
                if (!allIfaces.contains(iface)) {
                    allIfaces.add(iface);
                }
            }
            c = c.getSuperclass();
        }
        return allIfaces;
    }

    private static boolean installComputerHook(final Object targetPms, final HmaPolicyManager policyManager, final ZygoteEntry parentEntry) {
        boolean hookedAny = false;
        Object outerPms = resolveOuterPms(targetPms);
        if (outerPms != null) {
            sOuterPmsRef = outerPms;
        }
        Object[] candidates = outerPms != null ? new Object[]{targetPms, outerPms} : new Object[]{targetPms};

        for (Object holder : candidates) {
            if (holder == null) continue;
            for (String fieldName : new String[]{"mLiveComputer", "mComputer"}) {
                try {
                    Field compField = findFieldInHierarchy(holder.getClass(), fieldName);
                    if (compField == null) continue;

                    Object origComp = compField.get(holder);
                    if (origComp == null) continue;
                    if (Proxy.isProxyClass(origComp.getClass())
                            && Proxy.getInvocationHandler(origComp) instanceof PackageManagerHook) {
                        hookedAny = true;
                        continue;
                    }

                    Class<?> compInterface = null;
                    for (Class<?> iface : collectAllInterfaces(origComp.getClass())) {
                        String name = iface.getName();
                        if ("com.android.server.pm.Computer".equals(name) || name.endsWith(".Computer")) {
                            compInterface = iface;
                            break;
                        }
                    }
                    if (compInterface == null) continue;

                    Object proxyComp = Proxy.newProxyInstance(
                        compInterface.getClassLoader() != null ? compInterface.getClassLoader() : origComp.getClass().getClassLoader(),
                        new Class<?>[]{compInterface},
                        new PackageManagerHook(origComp, targetPms, policyManager, parentEntry, true)
                    );
                    if (compField.getType().isAssignableFrom(proxyComp.getClass())) {
                        compField.set(holder, proxyComp);
                        hookedAny = true;
                    }
                } catch (Throwable t) {
                    System.out.println(TAG + " Computer dynamic proxy hook non-fatal: " + t.getMessage());
                }
            }
        }
        return hookedAny;
    }

    private static boolean installAppsFilterHook(final Object targetPms, final HmaPolicyManager policyManager, final ZygoteEntry parentEntry) {
        boolean hookedAny = false;
        Object outerPms = resolveOuterPms(targetPms);
        Object[] candidates = outerPms != null ? new Object[]{targetPms, outerPms} : new Object[]{targetPms};

        for (Object holder : candidates) {
            if (holder == null) continue;
            try {
                Field appsFilterField = findFieldInHierarchy(holder.getClass(), "mAppsFilter");
                if (appsFilterField == null) continue;

                final Object origAppsFilter = appsFilterField.get(holder);
                if (origAppsFilter == null) continue;
                if (Proxy.isProxyClass(origAppsFilter.getClass())) {
                    hookedAny = true;
                    continue;
                }

                Class<?> snapshotInterface = null;
                for (Class<?> iface : collectAllInterfaces(origAppsFilter.getClass())) {
                    if (iface.getName().contains("AppsFilterSnapshot") || iface.getName().contains("AppsFilter")) {
                        snapshotInterface = iface;
                        break;
                    }
                }
                if (snapshotInterface == null) continue;

                Object proxyAppsFilter = Proxy.newProxyInstance(
                    snapshotInterface.getClassLoader() != null ? snapshotInterface.getClassLoader() : origAppsFilter.getClass().getClassLoader(),
                    new Class<?>[]{snapshotInterface},
                    new InvocationHandler() {
                        @Override
                        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
                            if ("shouldFilterApplication".equals(method.getName()) && args != null && !Boolean.TRUE.equals(IN_HOOK.get())) {
                                IN_HOOK.set(Boolean.TRUE);
                                try {
                                    int callingUid = extractCallingUidFromArgs(args);
                                    String targetPkg = extractTargetPackageFromArgs(args);
                                    if (callingUid >= HmaConstants.FIRST_APPLICATION_UID && targetPkg != null) {
                                        String[] callingPkgs = extractCallerPackages(targetPms, callingUid, args);
                                        parentEntry.notifyCallerPolicyApplied();
                                        if (policyManager.shouldHide(callingUid, callingPkgs, targetPkg)) {
                                            parentEntry.notifyResponseFiltered();
                                            return Boolean.TRUE; // true = filter (hide) from caller
                                        }
                                    }
                                } finally {
                                    IN_HOOK.set(Boolean.FALSE);
                                }
                            }
                            return method.invoke(origAppsFilter, args);
                        }
                    }
                );

                // Only replace field when compatible with declared field type (e.g. AppsFilterSnapshot interface)
                if (appsFilterField.getType().isAssignableFrom(proxyAppsFilter.getClass())) {
                    appsFilterField.set(holder, proxyAppsFilter);
                    hookedAny = true;
                }
            } catch (Throwable t) {
                System.out.println(TAG + " AppsFilter dynamic proxy hook non-fatal: " + t.getMessage());
            }
        }
        return hookedAny;
    }

    private static Object createBinderWrapper(final Object originalBinder, final Object proxyInterface) {
        try {
            Class<?> ibinderClass = Class.forName("android.os.IBinder");
            return Proxy.newProxyInstance(
                ibinderClass.getClassLoader() != null ? ibinderClass.getClassLoader() : originalBinder.getClass().getClassLoader(),
                new Class<?>[]{ibinderClass},
                new InvocationHandler() {
                    @Override
                    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
                        String name = method.getName();
                        if ("queryLocalInterface".equals(name) && args != null && args.length >= 1) {
                            Object arg0 = args[0];
                            if ("android.content.pm.IPackageManager".equals(arg0)) {
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
        if (Boolean.TRUE.equals(IN_HOOK.get())) {
            return invokeOriginal(method, args);
        }

        if (!isComputerProxy && sOuterPmsRef != null) {
            installComputerHook(originalPms, policyManager, parentEntry);
        }

        String methodName = method.getName();
        int callingUid = getCallingUid();
        if (isComputerProxy && callingUid < HmaConstants.FIRST_APPLICATION_UID && args != null) {
            if ("filterAppAccess".equals(methodName) && args.length >= 2 && args[1] instanceof Integer) {
                callingUid = ((Integer) args[1]).intValue();
            } else {
                int argUid = extractCallingUidFromArgs(args);
                if (argUid >= HmaConstants.FIRST_APPLICATION_UID) {
                    callingUid = argUid;
                }
            }
        }

        IN_HOOK.set(Boolean.TRUE);
        try {
            if ("filterAppAccess".equals(methodName) && args != null && args.length >= 1) {
                String targetPkg = extractFirstStringArg(args);
                if (targetPkg == null) {
                    targetPkg = extractSettingPackageName(args[0]);
                }
                if (callingUid >= HmaConstants.FIRST_APPLICATION_UID && targetPkg != null) {
                    String[] callingPkgs = getPackagesForUidRaw(uidLookupPms, callingUid);
                    parentEntry.notifyCallerPolicyApplied();
                    if (policyManager.shouldHide(callingUid, callingPkgs, targetPkg)) {
                        parentEntry.notifyResponseFiltered();
                        return Boolean.TRUE;
                    }
                }
                return invokeOriginal(method, args);
            }

            // 1. Single Package Queries: getPackageInfo*, getPackageInfoVersioned, getApplicationInfo*, getPackageUid
            if ("getPackageInfo".equals(methodName) || "getPackageInfoInternal".equals(methodName)
                    || "getApplicationInfo".equals(methodName) || "getApplicationInfoInternal".equals(methodName)
                    || "getPackageUid".equals(methodName)) {
                String targetPkg = extractFirstStringArg(args);
                if (targetPkg != null) {
                    String[] callingPkgs = getPackagesForUidRaw(uidLookupPms, callingUid);
                    parentEntry.notifyCallerPolicyApplied();
                    if (policyManager.shouldHide(callingUid, callingPkgs, targetPkg)) {
                        parentEntry.notifyResponseFiltered();
                        if ("getPackageUid".equals(methodName)) {
                            return Integer.valueOf(-1);
                        }
                        return null;
                    }
                }
            } else if ("getPackageInfoVersioned".equals(methodName) && args != null && args.length >= 1 && args[0] != null) {
                String targetPkg = extractVersionedPackageName(args[0]);
                if (targetPkg != null) {
                    String[] callingPkgs = getPackagesForUidRaw(uidLookupPms, callingUid);
                    parentEntry.notifyCallerPolicyApplied();
                    if (policyManager.shouldHide(callingUid, callingPkgs, targetPkg)) {
                        parentEntry.notifyResponseFiltered();
                        return null;
                    }
                }
            }

            // 2. Collection Queries: getInstalledPackages, getInstalledApplications, getPackagesHoldingPermissions
            if ("getInstalledPackages".equals(methodName) || "getInstalledApplications".equals(methodName)
                    || "getPackagesHoldingPermissions".equals(methodName)) {
                String[] callingPkgs = getPackagesForUidRaw(uidLookupPms, callingUid);
                parentEntry.notifyCallerPolicyApplied();
                Object result = invokeOriginal(method, args);
                return filterCollectionResult(result, callingUid, callingPkgs);
            }

            // 3. Intent Resolution Vectors
            if (methodName.startsWith("queryIntent") || methodName.startsWith("queryPersistent")) {
                String[] callingPkgs = getPackagesForUidRaw(uidLookupPms, callingUid);
                parentEntry.notifyCallerPolicyApplied();
                Object result = invokeOriginal(method, args);
                return filterCollectionResult(result, callingUid, callingPkgs);
            }

            if ("resolveIntent".equals(methodName) || "resolveService".equals(methodName)
                    || "resolveContentProvider".equals(methodName)) {
                String[] callingPkgs = getPackagesForUidRaw(uidLookupPms, callingUid);
                parentEntry.notifyCallerPolicyApplied();
                Object result = invokeOriginal(method, args);
                if (result != null) {
                    String pkg = extractPackageName(result);
                    if (pkg != null && policyManager.shouldHide(callingUid, callingPkgs, pkg)) {
                        parentEntry.notifyResponseFiltered();
                        return null;
                    }
                }
                return result;
            }

            // 4. Installer Spoofing: getInstallerPackageName & getInstallSourceInfo (Android 11+)
            if ("getInstallerPackageName".equals(methodName)) {
                String targetPkg = extractFirstStringArg(args);
                if (targetPkg != null) {
                    String[] callingPkgs = getPackagesForUidRaw(uidLookupPms, callingUid);
                    if (policyManager.shouldHide(callingUid, callingPkgs, targetPkg)) {
                        parentEntry.notifyResponseFiltered();
                        return null;
                    }
                    String spoofed = policyManager.getSpoofedInstaller(callingUid, callingPkgs, targetPkg);
                    if (spoofed != null) {
                        parentEntry.notifyResponseFiltered();
                        return spoofed;
                    }
                    Object realInstaller = invokeOriginal(method, args);
                    if (realInstaller instanceof String
                            && policyManager.shouldHide(callingUid, callingPkgs, (String) realInstaller)) {
                        parentEntry.notifyResponseFiltered();
                        return null;
                    }
                    return realInstaller;
                }
            }

            if ("getInstallSourceInfo".equals(methodName)) {
                String targetPkg = extractFirstStringArg(args);
                if (targetPkg != null) {
                    String[] callingPkgs = getPackagesForUidRaw(uidLookupPms, callingUid);
                    if (policyManager.shouldHide(callingUid, callingPkgs, targetPkg)) {
                        parentEntry.notifyResponseFiltered();
                        return null;
                    }
                    Object origInfo = invokeOriginal(method, args);
                    String spoofed = policyManager.getSpoofedInstaller(callingUid, callingPkgs, targetPkg);
                    if (spoofed != null) {
                        Object rewritten = buildSpoofedInstallSourceInfo(origInfo, spoofed);
                        if (rewritten != null) {
                            parentEntry.notifyResponseFiltered();
                            return rewritten;
                        }
                    } else if (origInfo != null) {
                        Object sanitized = sanitizeHiddenInstallSourceInfo(origInfo, callingUid, callingPkgs);
                        if (sanitized != origInfo) {
                            parentEntry.notifyResponseFiltered();
                            return sanitized;
                        }
                    }
                    return origInfo;
                }
            }

            // 5. Packages for UID Query
            if ("getPackagesForUid".equals(methodName) && args != null && args.length >= 1 && args[0] instanceof Integer) {
                String[] callingPkgs = getPackagesForUidRaw(uidLookupPms, callingUid);
                Object result = invokeOriginal(method, args);
                if (result instanceof String[]) {
                    String[] pkgs = (String[]) result;
                    List<String> visible = new ArrayList<String>();
                    boolean modified = false;
                    for (String p : pkgs) {
                        if (policyManager.shouldHide(callingUid, callingPkgs, p)) {
                            modified = true;
                            parentEntry.notifyResponseFiltered();
                        } else {
                            visible.add(p);
                        }
                    }
                    if (!modified) return result;
                    return visible.isEmpty() ? null : visible.toArray(new String[0]);
                }
                return result;
            }

            if ("getPackageInfo".equals(methodName)
                    || "getPackageInfoInternal".equals(methodName)
                    || "getPackageInfoVersioned".equals(methodName)) {
                Object result = invokeOriginal(method, args);
                if (result != null && FakeGAppsSignatureSpoofer.isEnabled()) {
                    long flags = extractPackageInfoFlags(methodName, args);
                    return FakeGAppsSignatureSpoofer.spoofPackageInfo(result, flags);
                }
                return result;
            }

            return invokeOriginal(method, args);
        } finally {
            IN_HOOK.set(Boolean.FALSE);
        }
    }

    private static long extractPackageInfoFlags(String methodName, Object[] args) {
        if (args == null) {
            return 0L;
        }
        int idx = "getPackageInfoInternal".equals(methodName) ? 2 : 1;
        if (idx >= args.length) {
            return 0L;
        }
        Object val = args[idx];
        if (val instanceof Long) {
            return ((Long) val).longValue();
        }
        if (val instanceof Integer) {
            return ((Integer) val).intValue() & 0xFFFFFFFFL;
        }
        return 0L;
    }

    private Object invokeOriginal(Method method, Object[] args) throws Throwable {
        try {
            return method.invoke(originalPms, args);
        } catch (java.lang.reflect.InvocationTargetException e) {
            throw e.getTargetException();
        }
    }

    private static Object buildSpoofedInstallSourceInfo(Object templateInfo, String spoofedInstaller) {
        try {
            Class<?> infoClass = (templateInfo != null)
                ? templateInfo.getClass()
                : Class.forName("android.content.pm.InstallSourceInfo");
            Class<?> signingInfoClass = Class.forName("android.content.pm.SigningInfo");
            // Android 13+ 5-arg constructor: (String initiating, SigningInfo signing, String originating, String installing, int packageSource)
            for (Constructor<?> ctor : infoClass.getConstructors()) {
                Class<?>[] params = ctor.getParameterTypes();
                if (params.length == 5
                        && params[0] == String.class
                        && params[1] == signingInfoClass
                        && params[2] == String.class
                        && params[3] == String.class
                        && params[4] == int.class) {
                    return ctor.newInstance(spoofedInstaller, null, null, spoofedInstaller, Integer.valueOf(2));
                }
                if (params.length == 4
                        && params[0] == String.class
                        && params[1] == signingInfoClass
                        && params[2] == String.class
                        && params[3] == String.class) {
                    return ctor.newInstance(spoofedInstaller, null, null, spoofedInstaller);
                }
            }
        } catch (Throwable ignored) {}
        return null;
    }

    private Object sanitizeHiddenInstallSourceInfo(Object origInfo, int callingUid, String[] callingPkgs) {
        try {
            Class<?> cls = origInfo.getClass();
            String initiating = (String) cls.getMethod("getInitiatingPackageName").invoke(origInfo);
            String originating = (String) cls.getMethod("getOriginatingPackageName").invoke(origInfo);
            String installing = (String) cls.getMethod("getInstallingPackageName").invoke(origInfo);

            boolean changed = false;
            if (initiating != null && policyManager.shouldHide(callingUid, callingPkgs, initiating)) {
                initiating = null;
                changed = true;
            }
            if (originating != null && policyManager.shouldHide(callingUid, callingPkgs, originating)) {
                originating = null;
                changed = true;
            }
            if (installing != null && policyManager.shouldHide(callingUid, callingPkgs, installing)) {
                installing = null;
                changed = true;
            }
            if (!changed) return origInfo;

            Class<?> signingInfoClass = Class.forName("android.content.pm.SigningInfo");
            for (Constructor<?> ctor : cls.getConstructors()) {
                Class<?>[] params = ctor.getParameterTypes();
                if (params.length == 5
                        && params[0] == String.class
                        && params[1] == signingInfoClass
                        && params[2] == String.class
                        && params[3] == String.class
                        && params[4] == int.class) {
                    return ctor.newInstance(initiating, null, originating, installing, Integer.valueOf(0));
                }
                if (params.length == 4
                        && params[0] == String.class
                        && params[1] == signingInfoClass
                        && params[2] == String.class
                        && params[3] == String.class) {
                    return ctor.newInstance(initiating, null, originating, installing);
                }
            }
        } catch (Throwable ignored) {}
        return origInfo;
    }

    private Object filterCollectionResult(Object result, int callingUid, String[] callingPkgs) {
        if (result == null) return null;

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

    private static String extractFirstStringArg(Object[] args) {
        if (args != null && args.length >= 1 && args[0] instanceof String) {
            return (String) args[0];
        }
        return null;
    }

    private static String extractVersionedPackageName(Object versionedPkg) {
        try {
            Method m = versionedPkg.getClass().getMethod("getPackageName");
            Object res = m.invoke(versionedPkg);
            if (res instanceof String) return (String) res;
        } catch (Throwable ignored) {}
        return null;
    }

    private static int extractCallingUidFromArgs(Object[] args) {
        for (Object arg : args) {
            if (arg instanceof Integer) {
                int val = ((Integer) arg).intValue();
                if (val >= 1000) return val;
            }
        }
        return -1;
    }

    private static String[] extractCallerPackages(Object targetPms, int callingUid, Object[] args) {
        String[] fromPms = getPackagesForUidRaw(targetPms, callingUid);
        if (fromPms != null && fromPms.length > 0) {
            return fromPms;
        }
        // In shouldFilterApplication(snapshot, callingUid, callingSetting, targetPkgSetting, userId),
        // args often contains callingSetting before targetPkgSetting.
        List<String> settingPkgs = new ArrayList<String>();
        for (Object arg : args) {
            String pkg = extractSettingPackageName(arg);
            if (pkg != null) settingPkgs.add(pkg);
        }
        if (settingPkgs.size() >= 2) {
            return new String[]{settingPkgs.get(0)};
        }
        return null;
    }

    private static String extractTargetPackageFromArgs(Object[] args) {
        // In shouldFilterApplication(..., callingSetting, targetPkgSetting, userId),
        // targetPkgSetting is the last package-setting argument.
        String lastMatch = null;
        for (Object arg : args) {
            String pkg = extractSettingPackageName(arg);
            if (pkg != null) {
                lastMatch = pkg;
            }
        }
        return lastMatch;
    }

    private static String extractSettingPackageName(Object arg) {
        if (arg == null || arg instanceof Integer || arg instanceof Boolean || arg instanceof String) {
            return null;
        }
        try {
            Method getPkgName = arg.getClass().getMethod("getPackageName");
            Object val = getPkgName.invoke(arg);
            if (val instanceof String) return (String) val;
        } catch (Throwable ignored) {}
        try {
            Method getName = arg.getClass().getMethod("getName");
            Object val = getName.invoke(arg);
            if (val instanceof String) return (String) val;
        } catch (Throwable ignored) {}
        return null;
    }

    private int getCallingUid() {
        try {
            Class<?> binderClass = Class.forName("android.os.Binder");
            Method getCallingUidMethod = binderClass.getDeclaredMethod("getCallingUid");
            return ((Integer) getCallingUidMethod.invoke(null)).intValue();
        } catch (Throwable t) {
            return -1;
        }
    }

    private static String[] getPackagesForUidRaw(Object pms, int uid) {
        if (pms == null || uid <= 0) return null;
        try {
            Method getPkgsMethod = pms.getClass().getMethod("getPackagesForUid", int.class);
            Object res = getPkgsMethod.invoke(pms, Integer.valueOf(uid));
            if (res instanceof String[]) return (String[]) res;
        } catch (Throwable ignored) {}
        return null;
    }

    private String extractPackageName(Object info) {
        if (info == null) return null;
        if (info instanceof String) return (String) info;
        try {
            Field pkgField = info.getClass().getField("packageName");
            Object v = pkgField.get(info);
            if (v instanceof String) return (String) v;
        } catch (Throwable ignored) {}

        for (String sub : new String[]{"activityInfo", "serviceInfo", "providerInfo"}) {
            try {
                Field f = info.getClass().getField(sub);
                Object nested = f.get(info);
                if (nested != null) {
                    Field pField = nested.getClass().getField("packageName");
                    Object v = pField.get(nested);
                    if (v instanceof String) return (String) v;
                }
            } catch (Throwable ignored) {}
        }

        return null;
    }
}
