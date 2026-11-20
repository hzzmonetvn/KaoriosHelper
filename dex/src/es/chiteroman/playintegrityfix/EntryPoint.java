/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * Adapted from osm0sis/PlayIntegrityFork (GPL-3.0-only) @ 23ec56c2440c4fd1a5e893a23ef7861d6366a828
 */

package es.chiteroman.playintegrityfix;

/**
 * DEX entrypoint invoked from PifEngine inside target GMS/DroidGuard processes
 * when spoofProvider=1 and TEE simulation is inactive.
 */
public final class EntryPoint {
    private EntryPoint() {}

    public static boolean initProvider() {
        return CustomProvider.install();
    }

    public static boolean isProviderInstalled() {
        return CustomProvider.isInstalled();
    }

    public static long getInterceptCount() {
        return CustomProvider.getInterceptCount();
    }
}
