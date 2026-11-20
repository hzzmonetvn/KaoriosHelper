#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 HzzMonet / Kaorios Project
"""Execute real compiled HMA Java bytecode against simulated system_server framework stubs."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def find_jdk_tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    for pattern in (
        f"/home/hzzmonet/.gradle/jdks/*/bin/{name}",
        f"/home/hzzmonet/.gradle/jdks/*/*/bin/{name}",
        f"/usr/lib/jvm/*/bin/{name}",
    ):
        for candidate in sorted(Path("/").glob(pattern.lstrip("/"))):
            if candidate.is_file():
                return str(candidate)
    return ""


JAVAC = find_jdk_tool("javac")
JAVA = find_jdk_tool("java")


ANDROID_STUBS = {
    "android/os/IBinder.java": """
package android.os;
public interface IBinder {
    Object queryLocalInterface(String descriptor);
}
""",
    "android/os/Binder.java": """
package android.os;
public class Binder {
    public static int sCallingUid = 1000;
    public static int getCallingUid() { return sCallingUid; }
}
""",
    "android/os/Bundle.java": """
package android.os;
import java.util.HashMap;
import java.util.Map;
public class Bundle {
    private final Map<String, String> values = new HashMap<String, String>();
    public Bundle() {}
    public Bundle(int capacity) {}
    public void putString(String key, String value) { values.put(key, value); }
    public String getString(String key) { return values.get(key); }
}
""",
    "android/os/ServiceManager.java": """
package android.os;
import java.util.HashMap;
import java.util.Map;
public class ServiceManager {
    public static Map<String, Object> sCache = new HashMap<String, Object>();
    public static Object sPackageService;
    public static Object checkService(String name) {
        return "package".equals(name) ? sPackageService : null;
    }
    public static Object getService(String name) {
        return checkService(name);
    }
}
""",
    "android/content/IContentProvider.java": """
package android.content;
import android.os.Bundle;
public interface IContentProvider {
    Bundle call(Object attributionSource, String authority, String method, String arg, Bundle extras);
}
""",
    "android/content/pm/SigningInfo.java": """
package android.content.pm;
public class SigningInfo {}
""",
    "android/content/pm/InstallSourceInfo.java": """
package android.content.pm;
public class InstallSourceInfo {
    private final String initiating;
    private final String originating;
    private final String installing;
    private final int source;
    public InstallSourceInfo(String initiating, SigningInfo si, String originating, String installing, int source) {
        this.initiating = initiating;
        this.originating = originating;
        this.installing = installing;
        this.source = source;
    }
    public String getInitiatingPackageName() { return initiating; }
    public String getOriginatingPackageName() { return originating; }
    public String getInstallingPackageName() { return installing; }
    public int getPackageSource() { return source; }
}
""",
    "android/content/pm/PackageInfo.java": """
package android.content.pm;
public class PackageInfo {
    public String packageName;
    public PackageInfo(String pkg) { this.packageName = pkg; }
}
""",
    "android/content/pm/ParceledListSlice.java": """
package android.content.pm;
import java.util.List;
public class ParceledListSlice {
    private final List<?> list;
    public ParceledListSlice(List<?> list) { this.list = list; }
    public List<?> getList() { return list; }
}
""",
    "android/content/pm/IPackageManager.java": """
package android.content.pm;
public interface IPackageManager {
    PackageInfo getPackageInfo(String packageName, long flags, int userId);
    ParceledListSlice getInstalledPackages(long flags, int userId);
    String getInstallerPackageName(String packageName);
    InstallSourceInfo getInstallSourceInfo(String packageName, int callingUid);
    String[] getPackagesForUid(int uid);
}
""",
    "android/app/ActivityThread.java": """
package android.app;
import android.content.IContentProvider;
import android.content.pm.IPackageManager;
import java.util.HashMap;
import java.util.Map;
public class ActivityThread {
    public static class ProviderRecord {
        public IContentProvider mProvider;
        public ProviderRecord(IContentProvider p) { this.mProvider = p; }
    }
    public static IPackageManager sPackageManager;
    public static ActivityThread sCurrent = new ActivityThread();
    public Map<String, Object> mProviderMap = new HashMap<String, Object>();
    public static IPackageManager getPackageManager() { return sPackageManager; }
    public static ActivityThread currentActivityThread() { return sCurrent; }
}
""",
    "com/google/android/gms/droidguard/DroidGuardProbe.java": """
package com.google.android.gms.droidguard;
import java.security.KeyStore;
public class DroidGuardProbe {
    public static boolean probeFails(KeyStore ks) throws Exception {
        try {
            ks.getCertificateChain("att-key");
            return false;
        } catch (UnsupportedOperationException expected) {
            return true;
        }
    }
}
""",
    "com/android/server/pm/Computer.java": """
package com.android.server.pm;
import android.content.pm.PackageInfo;
import android.content.pm.ParceledListSlice;
public interface Computer {
    boolean filterAppAccess(String packageName, int callingUid, int userId);
    PackageInfo getPackageInfoInternal(String packageName, long versionCode, long flags, int filterCallingUid, int userId);
    ParceledListSlice getInstalledPackages(long flags, int userId);
}
""",
    "HmaJvmHarness.java": """
import android.app.ActivityThread;
import android.content.IContentProvider;
import android.content.pm.IPackageManager;
import android.content.pm.InstallSourceInfo;
import android.content.pm.PackageInfo;
import android.content.pm.ParceledListSlice;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.ServiceManager;
import com.android.server.pm.Computer;
import com.google.android.gms.droidguard.DroidGuardProbe;
import com.v7878.zygisk.EntryPoint;
import org.frknkrc44.hma_oss.zygote.HmaConstants;
import org.frknkrc44.hma_oss.zygote.ZygoteEntry;
import java.security.Key;
import java.security.KeyStore;
import java.security.KeyStoreSpi;
import java.security.Provider;
import java.security.Security;
import java.security.cert.Certificate;
import java.util.Arrays;
import java.util.Collections;
import java.util.Date;
import java.util.Enumeration;
import java.util.List;

public class HmaJvmHarness {
    public interface AppsFilterSnapshot {
        boolean shouldFilterApplication(Object snapshot, int callingUid, Object callerSetting, Object targetSetting, int userId);
    }

    public static class FakePkgSetting {
        private final String pkg;
        public FakePkgSetting(String pkg) { this.pkg = pkg; }
        public String getPackageName() { return pkg; }
    }

    public static class OuterPms {
        public AppsFilterSnapshot mAppsFilter = new AppsFilterSnapshot() {
            @Override
            public boolean shouldFilterApplication(Object snapshot, int callingUid, Object callerSetting, Object targetSetting, int userId) {
                return false;
            }
        };

        public Computer mLiveComputer = new Computer() {
            @Override
            public boolean filterAppAccess(String packageName, int callingUid, int userId) {
                return false;
            }
            @Override
            public PackageInfo getPackageInfoInternal(String packageName, long versionCode, long flags, int filterCallingUid, int userId) {
                return new PackageInfo(packageName);
            }
            @Override
            public ParceledListSlice getInstalledPackages(long flags, int userId) {
                return new ParceledListSlice(Arrays.asList(
                    new PackageInfo("com.topjohnwu.magisk"),
                    new PackageInfo("com.secret.wallet"),
                    new PackageInfo("org.lsposed.manager"),
                    new PackageInfo("com.normal.calculator")
                ));
            }
        };

        public class InnerPmsImpl implements IPackageManager, IBinder {
            @Override
            public Object queryLocalInterface(String descriptor) {
                return this;
            }
            @Override
            public PackageInfo getPackageInfo(String packageName, long flags, int userId) {
                return new PackageInfo(packageName);
            }
            @Override
            public ParceledListSlice getInstalledPackages(long flags, int userId) {
                return new ParceledListSlice(Arrays.asList(
                    new PackageInfo("com.topjohnwu.magisk"),
                    new PackageInfo("com.secret.wallet"),
                    new PackageInfo("com.bank.client"),
                    new PackageInfo("com.normal.calculator")
                ));
            }
            @Override
            public String getInstallerPackageName(String packageName) {
                return "com.topjohnwu.magisk";
            }
            @Override
            public InstallSourceInfo getInstallSourceInfo(String packageName, int callingUid) {
                return new InstallSourceInfo("com.topjohnwu.magisk", null, "com.topjohnwu.magisk", "com.topjohnwu.magisk", 0);
            }
            @Override
            public String[] getPackagesForUid(int uid) {
                if (uid == 10200) return new String[]{"com.bank.client"};
                if (uid == 10300) return new String[]{"com.strict.client"};
                if (uid == 10400) return new String[]{"com.topjohnwu.magisk"};
                return new String[]{"com.other.app"};
            }
        }
    }

    private static void check(boolean cond, String msg) {
        if (!cond) throw new AssertionError(msg);
    }

    public static void main(String[] args) throws Exception {
        System.setProperty("kaorios.hma.disable_deferred_bootstrap", "true");

        OuterPms outer = new OuterPms();
        OuterPms.InnerPmsImpl inner = outer.new InnerPmsImpl();
        ActivityThread.sPackageManager = inner;
        ServiceManager.sPackageService = inner;
        ServiceManager.sCache.put("package", inner);

        IContentProvider origSettings = new IContentProvider() {
            @Override
            public Bundle call(Object attr, String authority, String method, String arg, Bundle extras) {
                Bundle b = new Bundle(1);
                b.putString("value", "1");
                return b;
            }
        };
        ActivityThread.ProviderRecord settingsRec = new ActivityThread.ProviderRecord(origSettings);
        ActivityThread.sCurrent.mProviderMap.put("settings", settingsRec);

        // Single-line compact JSON covering templates, presets, ignoredPackagesForPresets, settingsTemplates, AppRule objects, extraOppositeAppList, whitelists, installer spoofs & dev settings
        String compactJson = "{\\"configVersion\\":1,"
            + "\\"ignoredPackagesForPresets\\":[\\"org.meowcat.edxposed.manager\\"],"
            + "\\"templates\\":{\\"HiddenTools\\":{\\"isWhitelist\\":false,\\"appList\\":[\\"com.secret.wallet\\"]}},"
            + "\\"settingsTemplates\\":{\\"DevHide\\":{\\"values\\":{\\"adb_enabled\\":\\"0\\",\\"custom_flag\\":\\"false\\"}}},"
            + "\\"whitelist\\":{\\"com.strict.client\\":[\\"com.normal.calculator\\"]},"
            + "\\"scope\\":{\\"com.bank.client\\":{\\"useWhitelist\\":false,\\"extraAppList\\":[],\\"applyTemplates\\":[\\"HiddenTools\\"],\\"applyPresets\\":[\\"xposed\\"],\\"extraOppositeAppList\\":[\\"com.topjohnwu.magisk\\"],\\"hideInstallationSource\\":true,\\"applySettingTemplates\\":[\\"DevHide\\"]}},"
            + "\\"installer_spoofs\\":{\\"com.normal.calculator\\":\\"com.android.vending\\"}}";

        // Invoke via preferred EntryPoint wrapper to prove full contract handoff
        EntryPoint.init(compactJson);
        check(EntryPoint.getHookState() == HmaConstants.STATE_APPSFILTER_HOOKED,
              "Expected STATE_APPSFILTER_HOOKED (6), got " + EntryPoint.getHookState());

        // 1. Test AppsFilterSnapshot inner-to-outer PMS hook with per-caller scope and extraOppositeAppList exemption!
        check(outer.mAppsFilter.shouldFilterApplication(null, 10200, new FakePkgSetting("com.bank.client"), new FakePkgSetting("com.secret.wallet"), 0),
              "AppsFilter must hide template package from scoped caller");
        check(!outer.mAppsFilter.shouldFilterApplication(null, 10200, new FakePkgSetting("com.bank.client"), new FakePkgSetting("com.topjohnwu.magisk"), 0),
              "extraOppositeAppList must exempt package from global/template blacklist for scoped caller");
        check(!outer.mAppsFilter.shouldFilterApplication(null, 10999, new FakePkgSetting("com.other.app"), new FakePkgSetting("com.secret.wallet"), 0),
              "AppsFilter must not hide scoped package from unrelated caller");
        check(!outer.mAppsFilter.shouldFilterApplication(null, 1000, new FakePkgSetting("android"), new FakePkgSetting("com.topjohnwu.magisk"), 0),
              "AppsFilter must never filter system UID 1000");

        // 2. Test IPackageManager proxy: ParceledListSlice filtering
        IPackageManager hookedPms = ActivityThread.getPackageManager();
        Binder.sCallingUid = 10200; // com.bank.client
        check(hookedPms.getPackageInfo("com.secret.wallet", 0L, 0) == null, "Scoped package must return null PackageInfo");
        check(hookedPms.getPackageInfo("com.bank.client", 0L, 0) != null, "Caller must see itself");

        ParceledListSlice slice = hookedPms.getInstalledPackages(0L, 0);
        List<?> items = slice.getList();
        check(items.size() == 3, "Expected 3 visible packages for com.bank.client (Magisk exempted via extraOppositeAppList), got " + items.size());

        // 3. Test whitelist caller
        Binder.sCallingUid = 10300; // com.strict.client
        ParceledListSlice wlSlice = hookedPms.getInstalledPackages(0L, 0);
        check(wlSlice.getList().size() == 1, "Whitelist caller must only see whitelisted calculator");

        // 4. Test getInstallSourceInfo & getInstallerPackageName spoofing
        Binder.sCallingUid = 10200;
        InstallSourceInfo isi = hookedPms.getInstallSourceInfo("com.normal.calculator", 10200);
        check("com.android.vending".equals(isi.getInstallingPackageName()),
              "InstallSourceInfo must spoof installingPackageName to Play Store");
        check("com.android.vending".equals(hookedPms.getInstallerPackageName("com.normal.calculator")),
              "getInstallerPackageName must return spoofed Play Store");

        // 5. Test getPackagesForUid returning null when all packages for queried UID are hidden
        Binder.sCallingUid = 10999; // unrelated caller without opposite exemption still sees global Magisk hidden
        check(hookedPms.getPackagesForUid(10400) == null,
              "getPackagesForUid must return null when all packages of target UID are hidden");

        // 6. Test SettingsProviderHook developer options / ADB & settingsTemplates spoofing
        Binder.sCallingUid = 10200; // com.bank.client
        Bundle bankAdb = settingsRec.mProvider.call(null, "settings", "GET_global", "adb_enabled", null);
        check("0".equals(bankAdb.getString("value")), "Scoped caller must receive adb_enabled=0");
        Bundle bankDev = settingsRec.mProvider.call(null, "settings", "GET_global", "development_settings_enabled", null);
        check("0".equals(bankDev.getString("value")), "Scoped caller must receive development_settings_enabled=0");
        Bundle customSetting = settingsRec.mProvider.call(null, "settings", "GET_secure", "custom_flag", null);
        check("false".equals(customSetting.getString("value")), "Scoped caller must receive settingsTemplates override custom_flag=false");

        Binder.sCallingUid = 1000; // system_server / Settings UI
        Bundle sysAdb = settingsRec.mProvider.call(null, "settings", "GET_global", "adb_enabled", null);
        check("1".equals(sysAdb.getString("value")), "System UID 1000 must still see real adb_enabled=1");

        // Verify state reached STATE_RESPONSE_FILTERED (8)
        check(EntryPoint.getHookState() == HmaConstants.STATE_RESPONSE_FILTERED,
              "Expected STATE_RESPONSE_FILTERED (8), got " + EntryPoint.getHookState());
        check(ZygoteEntry.getFilterCount() > 0, "Expected positive filter count");

        // 7. Test com.android.server.pm.Computer remote-Binder proxy + applyPresets + ignoredPackagesForPresets
        check(outer.mLiveComputer.filterAppAccess("org.lsposed.manager", 10200, 0),
              "Computer proxy must hide preset xposed package from scoped caller");
        check(!outer.mLiveComputer.filterAppAccess("org.meowcat.edxposed.manager", 10200, 0),
              "ignoredPackagesForPresets must exempt excluded package from applyPresets");
        Binder.sCallingUid = 10200;
        check(outer.mLiveComputer.getInstalledPackages(0L, 0).getList().size() == 2,
              "Computer getInstalledPackages must filter secret.wallet and lsposed manager for com.bank.client");

        // 8. Test PlayIntegrityFork CustomProvider / CustomKeyStoreSpi
        Security.removeProvider("AndroidKeyStore");
        Provider stubAks = new Provider("AndroidKeyStore", 1.0, "Stub AndroidKeyStore") {};
        stubAks.put("KeyStore.AndroidKeyStore", StubKeyStoreSpi.class.getName());
        Security.insertProviderAt(stubAks, 1);

        check(es.chiteroman.playintegrityfix.EntryPoint.initProvider(),
              "PIF EntryPoint.initProvider() must install CustomProvider at priority 1");
        check(es.chiteroman.playintegrityfix.EntryPoint.isProviderInstalled(),
              "CustomProvider must remain installed");
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        Certificate[] normalChain = ks.getCertificateChain("att-key");
        check(normalChain != null && normalChain.length == 1,
              "Non-DroidGuard caller must receive real certificate chain from delegate");
        check(es.chiteroman.playintegrityfix.EntryPoint.getInterceptCount() == 0,
              "Non-DroidGuard call must not increment intercept count");
        check(DroidGuardProbe.probeFails(ks),
              "DroidGuard caller must throw UnsupportedOperationException");
        check(es.chiteroman.playintegrityfix.EntryPoint.getInterceptCount() == 1,
              "DroidGuard call must increment CustomProvider intercept count");

        System.out.println("HMA_JVM_HARNESS_PASS");
    }

    public static final class StubKeyStoreSpi extends KeyStoreSpi {
        private static final Certificate DUMMY_CERT = new Certificate("X.509") {
            @Override public byte[] getEncoded() { return new byte[]{1, 2, 3}; }
            @Override public void verify(java.security.PublicKey key) {}
            @Override public void verify(java.security.PublicKey key, String sigProvider) {}
            @Override public String toString() { return "DummyCert"; }
            @Override public java.security.PublicKey getPublicKey() { return null; }
        };
        @Override public Key engineGetKey(String alias, char[] password) { return null; }
        @Override public Certificate[] engineGetCertificateChain(String alias) { return new Certificate[]{DUMMY_CERT}; }
        @Override public Certificate engineGetCertificate(String alias) { return DUMMY_CERT; }
        @Override public Date engineGetCreationDate(String alias) { return new Date(0); }
        @Override public void engineSetKeyEntry(String alias, Key key, char[] password, Certificate[] chain) {}
        @Override public void engineSetKeyEntry(String alias, byte[] key, Certificate[] chain) {}
        @Override public void engineSetCertificateEntry(String alias, Certificate cert) {}
        @Override public void engineDeleteEntry(String alias) {}
        @Override public Enumeration<String> engineAliases() { return Collections.emptyEnumeration(); }
        @Override public boolean engineContainsAlias(String alias) { return true; }
        @Override public int engineSize() { return 1; }
        @Override public boolean engineIsKeyEntry(String alias) { return false; }
        @Override public boolean engineIsCertificateEntry(String alias) { return true; }
        @Override public String engineGetCertificateAlias(Certificate cert) { return "att-key"; }
        @Override public void engineStore(java.io.OutputStream stream, char[] password) {}
        @Override public void engineLoad(java.io.InputStream stream, char[] password) {}
    }
}
""",
}


@unittest.skipUnless(JAVAC and JAVA, "JDK javac/java required for HMA JVM bytecode contract test")
class TestHmaDexRuntime(unittest.TestCase):
    def test_hma_bytecode_full_framework_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            stub_src = tmp_path / "src"
            classes_out = tmp_path / "classes"
            classes_out.mkdir(parents=True)

            for rel, content in ANDROID_STUBS.items():
                target = stub_src / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content.strip() + "\n")

            java_sources = sorted(
                [str(p) for p in (ROOT / "dex/src").rglob("*.java")]
                + [str(p) for p in stub_src.rglob("*.java")]
            )
            compile_res = subprocess.run(
                [JAVAC, "--release", "8", "-d", str(classes_out), *java_sources],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(compile_res.returncode, 0, compile_res.stderr)

            run_res = subprocess.run(
                [JAVA, "-cp", str(classes_out), "HmaJvmHarness"],
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertEqual(run_res.returncode, 0, run_res.stderr + "\n" + run_res.stdout)
            self.assertIn("HMA_JVM_HARNESS_PASS", run_res.stdout)


if __name__ == "__main__":
    unittest.main()
