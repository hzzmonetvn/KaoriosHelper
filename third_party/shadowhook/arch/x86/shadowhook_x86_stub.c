/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * x86 emulator fallback for ShadowHook (Inline hooking requires ARM/ARM64 on physical hardware)
 */

#include "shadowhook.h"
#include <stddef.h>

const char *shadowhook_get_version(void) {
    return "2.0.1-x86-stub";
}

int shadowhook_init(shadowhook_mode_t default_mode, bool debuggable) {
    (void)default_mode;
    (void)debuggable;
    return 0;
}

int shadowhook_get_init_errno(void) {
    return 0;
}

shadowhook_mode_t shadowhook_get_mode(void) {
    return SHADOWHOOK_MODE_UNIQUE;
}

bool shadowhook_get_debuggable(void) {
    return false;
}

void shadowhook_set_debuggable(bool debuggable) {
    (void)debuggable;
}

bool shadowhook_get_recordable(void) {
    return false;
}

void shadowhook_set_recordable(bool recordable) {
    (void)recordable;
}

bool shadowhook_get_disable(void) {
    return false;
}

void shadowhook_set_disable(bool disable) {
    (void)disable;
}

int shadowhook_get_errno(void) {
    return 0;
}

const char *shadowhook_to_errmsg(int error_number) {
    (void)error_number;
    return "OK";
}

void *shadowhook_hook_func_addr(void *func_addr, void *new_addr, void **orig_addr) {
    (void)func_addr;
    (void)new_addr;
    (void)orig_addr;
    return NULL;
}

void *shadowhook_hook_sym_addr(void *sym_addr, void *new_addr, void **orig_addr) {
    (void)sym_addr;
    (void)new_addr;
    (void)orig_addr;
    return NULL;
}

void *shadowhook_hook_sym_name(const char *lib_name, const char *sym_name, void *new_addr, void **orig_addr) {
    (void)lib_name;
    (void)sym_name;
    (void)new_addr;
    (void)orig_addr;
    return NULL;
}

void *shadowhook_hook_sym_name_callback(const char *lib_name, const char *sym_name, void *new_addr,
                                        void **orig_addr, shadowhook_hooked_t hooked, void *hooked_arg) {
    (void)lib_name;
    (void)sym_name;
    (void)new_addr;
    (void)orig_addr;
    (void)hooked;
    (void)hooked_arg;
    return NULL;
}

int shadowhook_unhook(void *stub) {
    (void)stub;
    return 0;
}

void *shadowhook_hook_func_addr_2(void *func_addr, void *new_addr, void **orig_addr, uint32_t flags, ...) {
    (void)func_addr; (void)new_addr; (void)orig_addr; (void)flags;
    return NULL;
}

void *shadowhook_hook_sym_addr_2(void *sym_addr, void *new_addr, void **orig_addr, uint32_t flags, ...) {
    (void)sym_addr; (void)new_addr; (void)orig_addr; (void)flags;
    return NULL;
}

void *shadowhook_hook_sym_name_2(const char *lib_name, const char *sym_name, void *new_addr, void **orig_addr, uint32_t flags) {
    (void)lib_name; (void)sym_name; (void)new_addr; (void)orig_addr; (void)flags;
    return NULL;
}

void *shadowhook_hook_sym_name_callback_2(const char *lib_name, const char *sym_name, void *new_addr,
                                          void **orig_addr, uint32_t flags, shadowhook_hooked_t hooked, void *hooked_arg) {
    (void)lib_name; (void)sym_name; (void)new_addr; (void)orig_addr; (void)flags; (void)hooked; (void)hooked_arg;
    return NULL;
}

void *shadowhook_intercept_instr_addr(void *instr_addr, shadowhook_interceptor_t pre, void *data, uint32_t flags, ...) {
    (void)instr_addr; (void)pre; (void)data; (void)flags;
    return NULL;
}

void *shadowhook_intercept_func_addr(void *func_addr, shadowhook_interceptor_t pre, void *data, uint32_t flags, ...) {
    (void)func_addr; (void)pre; (void)data; (void)flags;
    return NULL;
}

void *shadowhook_intercept_sym_addr(void *sym_addr, shadowhook_interceptor_t pre, void *data, uint32_t flags, ...) {
    (void)sym_addr; (void)pre; (void)data; (void)flags;
    return NULL;
}

void *shadowhook_intercept_sym_name(const char *lib_name, const char *sym_name, shadowhook_interceptor_t pre, void *data, uint32_t flags) {
    (void)lib_name; (void)sym_name; (void)pre; (void)data; (void)flags;
    return NULL;
}
