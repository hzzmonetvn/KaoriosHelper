/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * Adapted from osm0sis/PlayIntegrityFork (GPL-3.0-only) @ 23ec56c2440c4fd1a5e893a23ef7861d6366a828
 */

package es.chiteroman.playintegrityfix;

import java.security.KeyStoreSpi;
import java.security.Provider;
import java.security.Security;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Security Provider replacing "AndroidKeyStore" at priority 1 inside GMS/DroidGuard processes
 * when PIF spoofProvider=1 and TEE simulation is inactive.
 */
public final class CustomProvider extends Provider {
    private static final long serialVersionUID = 1L;
    private static final String ANDROID_KEYSTORE = "AndroidKeyStore";
    private static final AtomicLong sInterceptCount = new AtomicLong(0);
    private static volatile Provider.Service sOriginalKeyStoreService = null;

    @SuppressWarnings("deprecation")
    private CustomProvider(Provider original) {
        super(original.getName(), original.getVersion(), original.getInfo());
        for (Map.Entry<Object, Object> entry : original.entrySet()) {
            String key = String.valueOf(entry.getKey());
            if (key.startsWith("Provider.id ") || "KeyStore.AndroidKeyStore".equals(key)) {
                continue;
            }
            put(entry.getKey(), entry.getValue());
        }
        put("KeyStore.AndroidKeyStore", CustomKeyStoreSpi.class.getName());
    }

    static KeyStoreSpi newOriginalKeyStoreSpi() {
        Provider.Service svc = sOriginalKeyStoreService;
        if (svc == null) {
            return null;
        }
        try {
            Object instance = svc.newInstance(null);
            if (instance instanceof KeyStoreSpi) {
                return (KeyStoreSpi) instance;
            }
        } catch (Throwable t) {
            System.err.println("[Kaorios/PIF] Failed to instantiate original AndroidKeyStore SPI: " + t.getMessage());
        }
        return null;
    }

    static void recordIntercept() {
        sInterceptCount.incrementAndGet();
    }

    public static long getInterceptCount() {
        return sInterceptCount.get();
    }

    public static boolean isInstalled() {
        return Security.getProvider(ANDROID_KEYSTORE) instanceof CustomProvider;
    }

    public static synchronized boolean install() {
        Provider existing = Security.getProvider(ANDROID_KEYSTORE);
        if (existing == null) {
            return false;
        }
        if (existing instanceof CustomProvider) {
            return true;
        }
        Provider.Service svc = existing.getService("KeyStore", ANDROID_KEYSTORE);
        if (svc == null) {
            return false;
        }
        sOriginalKeyStoreService = svc;
        CustomProvider replacement = new CustomProvider(existing);
        Security.removeProvider(ANDROID_KEYSTORE);
        int pos = Security.insertProviderAt(replacement, 1);
        return pos == 1 || isInstalled();
    }
}
