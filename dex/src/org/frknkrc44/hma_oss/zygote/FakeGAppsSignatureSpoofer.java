/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

package org.frknkrc44.hma_oss.zygote;

import java.lang.reflect.Array;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;

/**
 * Spoofs {@code PackageInfo.signatures} and {@code PackageInfo.signingInfo} for
 * {@code com.google.android.gms} and {@code com.android.vending} using the official
 * Google Play Services X.509 certificate (FakeGApps / microG compatibility).
 *
 * <p>Uses pure reflection so {@code classes.dex} compiles without compile-time
 * Android framework stubs and fails closed on any unexpected error.
 */
public final class FakeGAppsSignatureSpoofer {
    private static final String GMS_PACKAGE = "com.google.android.gms";
    private static final String VENDING_PACKAGE = "com.android.vending";
    private static final long FLAG_GET_SIGNATURES = 0x00000040L;
    private static final long FLAG_GET_SIGNING_CERTIFICATES = 0x08000000L;
    private static final int SIGNING_BLOCK_V3 = 3;

    // Official Google Play Services signing certificate (1095B DER,
    // SHA-1: 38918a453d07199354f8b19af05ec6562ced5788,
    // SHA-256: f0fd6c5b410f25cb25c3b53346c8972fae30f8ee7411df910480ad6b2d60db83).
    private static final String FAKE_GMS_SIGNATURE_HEX =
            "308204433082032ba003020102020900c2e08746644a308d300d06092a864886f70d010104050030"
            + "74310b3009060355040613025553311330110603550408130a43616c69666f726e69613116301406"
            + "03550407130d4d6f756e7461696e205669657731143012060355040a130b476f6f676c6520496e63"
            + "2e3110300e060355040b1307416e64726f69643110300e06035504031307416e64726f6964301e17"
            + "0d3038303832313233313333345a170d3336303130373233313333345a3074310b30090603550406"
            + "13025553311330110603550408130a43616c69666f726e6961311630140603550407130d4d6f756e"
            + "7461696e205669657731143012060355040a130b476f6f676c6520496e632e3110300e060355040b"
            + "1307416e64726f69643110300e06035504031307416e64726f696430820120300d06092a864886f7"
            + "0d01010105000382010d00308201080282010100ab562e00d83ba208ae0a966f124e29da11f2ab56"
            + "d08f58e2cca91303e9b754d372f640a71b1dcb130967624e4656a7776a92193db2e5bfb724a91e77"
            + "188b0e6a47a43b33d9609b77183145ccdf7b2e586674c9e1565b1f4c6a5955bff251a63dabf9c55c"
            + "27222252e875e4f8154a645f897168c0b1bfc612eabf785769bb34aa7984dc7e2ea2764cae8307d8"
            + "c17154d7ee5f64a51a44a602c249054157dc02cd5f5c0e55fbef8519fbe327f0b1511692c5a06f19"
            + "d18385f5c4dbc2d6b93f68cc2979c70e18ab93866b3bd5db8999552a0e3b4c99df58fb918bedc182"
            + "ba35e003c1b4b10dd244a8ee24fffd333872ab5221985edab0fc0d0b145b6aa192858e79020103a3"
            + "81d93081d6301d0603551d0e04160414c77d8cc2211756259a7fd382df6be398e4d786a53081a606"
            + "03551d2304819e30819b8014c77d8cc2211756259a7fd382df6be398e4d786a5a178a4763074310b"
            + "3009060355040613025553311330110603550408130a43616c69666f726e69613116301406035504"
            + "07130d4d6f756e7461696e205669657731143012060355040a130b476f6f676c6520496e632e3110"
            + "300e060355040b1307416e64726f69643110300e06035504031307416e64726f6964820900c2e087"
            + "46644a308d300c0603551d13040530030101ff300d06092a864886f70d010104050003820101006d"
            + "d252ceef85302c360aaace939bcff2cca904bb5d7a1661f8ae46b2994204d0ff4a68c7ed1a531ec4"
            + "595a623ce60763b167297a7ae35712c407f208f0cb109429124d7b106219c084ca3eb3f9ad5fb871"
            + "ef92269a8be28bf16d44c8d9a08e6cb2f005bb3fe2cb96447e868e731076ad45b33f6009ea19c161"
            + "e62641aa99271dfd5228c5c587875ddb7f452758d661f6cc0cccb7352e424cc4365c523532f73251"
            + "37593c4ae341f4db41edda0d0b1071a7cd0f42d4fb65d9098ca9b5b6bc7c5e56577d3bf78803c173"
            + "61932e58c8fa230e8a12438326d5c1fb14242f6110e589e47f1af84b96cd87aa9d03f6ef0c5b6dd8"
            + "9f3d93f7aa473b84894f632744171f241f013627097024189d8b35e78c47a7489643a2417b249a16"
            + "2655726b0786c1143069f5d79b134edae947d04e05cb1b2c7c886a5e44cd947e6cc3b8fb900cc228"
            + "05196162aa132d215499de35dd00c97e91";

    private static volatile boolean sEnabled = false;
    private static volatile Object sFakeSignature = null;
    private static volatile Object sFakeSignatureArray = null;
    private static volatile Object sFakeSigningInfo = null;
    private static volatile boolean sSignatureReflectionFailed = false;
    private static volatile boolean sSigningInfoReflectionFailed = false;

    private FakeGAppsSignatureSpoofer() {}

    public static void setEnabled(boolean enabled) {
        sEnabled = enabled;
    }

    public static boolean isEnabled() {
        return sEnabled;
    }

    public static Object spoofPackageInfo(Object pi, long flags) {
        if (!sEnabled || pi == null) {
            return pi;
        }
        try {
            Class<?> piClass = pi.getClass();
            Field pkgField = piClass.getField("packageName");
            Object pkgObj = pkgField.get(pi);
            if (!(pkgObj instanceof String)) {
                return pi;
            }
            String pkgName = (String) pkgObj;
            if (!GMS_PACKAGE.equals(pkgName) && !VENDING_PACKAGE.equals(pkgName)) {
                return pi;
            }

            Field signaturesField = piClass.getField("signatures");
            Object existingSigs = signaturesField.get(pi);
            if ((flags & FLAG_GET_SIGNATURES) != 0 || existingSigs != null) {
                Object fakeArr = getFakeSignatureArray();
                if (fakeArr != null) {
                    signaturesField.set(pi, fakeArr);
                }
            }

            try {
                Field signingInfoField = piClass.getField("signingInfo");
                Object existingInfo = signingInfoField.get(pi);
                if ((flags & FLAG_GET_SIGNING_CERTIFICATES) != 0 || existingInfo != null) {
                    Object fakeInfo = getFakeSigningInfo();
                    if (fakeInfo != null) {
                        signingInfoField.set(pi, fakeInfo);
                    }
                }
            } catch (NoSuchFieldException ignored) {
                // Pre-API 28 PackageInfo has no signingInfo field.
            }
        } catch (Throwable ignored) {
            // Fail closed: preserve original PackageInfo.
        }
        return pi;
    }

    private static Object getFakeSignature() {
        if (sSignatureReflectionFailed) {
            return null;
        }
        Object cached = sFakeSignature;
        if (cached != null) {
            return cached;
        }
        synchronized (FakeGAppsSignatureSpoofer.class) {
            if (sSignatureReflectionFailed) {
                return null;
            }
            if (sFakeSignature != null) {
                return sFakeSignature;
            }
            try {
                Class<?> sigClass = Class.forName("android.content.pm.Signature");
                Constructor<?> ctor = sigClass.getConstructor(String.class);
                Object sig = ctor.newInstance(FAKE_GMS_SIGNATURE_HEX);
                Object arr = Array.newInstance(sigClass, 1);
                Array.set(arr, 0, sig);
                sFakeSignature = sig;
                sFakeSignatureArray = arr;
                return sig;
            } catch (Throwable t) {
                sSignatureReflectionFailed = true;
                return null;
            }
        }
    }

    private static Object getFakeSignatureArray() {
        if (getFakeSignature() == null) {
            return null;
        }
        return sFakeSignatureArray;
    }

    private static Object getFakeSigningInfo() {
        if (sSigningInfoReflectionFailed) {
            return null;
        }
        Object cached = sFakeSigningInfo;
        if (cached != null) {
            return cached;
        }
        synchronized (FakeGAppsSignatureSpoofer.class) {
            if (sSigningInfoReflectionFailed) {
                return null;
            }
            if (sFakeSigningInfo != null) {
                return sFakeSigningInfo;
            }
            try {
                Object sigArr = getFakeSignatureArray();
                if (sigArr == null) {
                    sSigningInfoReflectionFailed = true;
                    return null;
                }
                Class<?> signingDetailsClass = Class.forName("android.content.pm.SigningDetails");
                Class<?> sigArrayClass = sigArr.getClass();
                Constructor<?> detailsCtor = signingDetailsClass.getConstructor(sigArrayClass, int.class);
                Object details = detailsCtor.newInstance(sigArr, Integer.valueOf(SIGNING_BLOCK_V3));
                Class<?> signingInfoClass = Class.forName("android.content.pm.SigningInfo");
                Constructor<?> infoCtor = signingInfoClass.getConstructor(signingDetailsClass);
                Object info = infoCtor.newInstance(details);
                sFakeSigningInfo = info;
                return info;
            } catch (Throwable t) {
                sSigningInfoReflectionFailed = true;
                return null;
            }
        }
    }
}
