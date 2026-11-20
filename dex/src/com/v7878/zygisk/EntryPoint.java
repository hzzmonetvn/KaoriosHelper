/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

package com.v7878.zygisk;

import org.frknkrc44.hma_oss.zygote.ZygoteEntry;

public class EntryPoint {
    public static void init(String configJson) {
        ZygoteEntry.init(configJson);
    }

    public static int getHookState() {
        return ZygoteEntry.getHookState();
    }

    public static String getHookStateName() {
        return ZygoteEntry.getHookStateName();
    }

    public static long getFilterCount() {
        return ZygoteEntry.getFilterCount();
    }

    public static boolean shouldHide(int callingUid, String targetPkg) {
        return ZygoteEntry.shouldHide(callingUid, targetPkg);
    }

    public static void main() {
        ZygoteEntry.main();
    }

    public static void main(String[] args) {
        main();
    }
}
