/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 */

package com.v7878.zygisk;

import org.frknkrc44.hma_oss.zygote.ZygoteEntry;

public class EntryPoint {
    public static void main() {
        ZygoteEntry.main();
    }

    public static void main(String[] args) {
        main();
    }
}
