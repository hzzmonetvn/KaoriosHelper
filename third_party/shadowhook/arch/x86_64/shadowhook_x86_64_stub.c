/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * x86_64 emulator fallback for ShadowHook (Inline hooking requires ARM64 on physical hardware)
 */

#include "shadowhook.h"
#include <stddef.h>

const char *shadowhook_get_version(void) {
    return "2.0.1-x86_64-stub";
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

void *shadowhook_intercept_sym_name_callback(const char *lib_name, const char *sym_name,
                                             shadowhook_interceptor_t pre, void *data, uint32_t flags,
                                             shadowhook_intercepted_t intercepted, void *intercepted_arg) {
    (void)lib_name; (void)sym_name; (void)pre; (void)data; (void)flags; (void)intercepted; (void)intercepted_arg;
    return NULL;
}

int shadowhook_unintercept(void *stub) {
    (void)stub;
    return 0;
}

char *shadowhook_get_records(uint32_t item_flags) {
    (void)item_flags;
    return NULL;
}

void shadowhook_dump_records(int fd, uint32_t item_flags) {
    (void)fd;
    (void)item_flags;
}

void *shadowhook_dlopen(const char *lib_name) {
    (void)lib_name;
    return NULL;
}

void shadowhook_dlclose(void *handle) {
    (void)handle;
}

void *shadowhook_dlsym(void *handle, const char *sym_name) {
    (void)handle;
    (void)sym_name;
    return NULL;
}

void *shadowhook_dlsym_dynsym(void *handle, const char *sym_name) {
    (void)handle;
    (void)sym_name;
    return NULL;
}

void *shadowhook_dlsym_symtab(void *handle, const char *sym_name) {
    (void)handle;
    (void)sym_name;
    return NULL;
}

int shadowhook_register_dl_init_callback(shadowhook_dl_info_t pre, shadowhook_dl_info_t post, void *data) {
    (void)pre; (void)post; (void)data;
    return 0;
}

int shadowhook_unregister_dl_init_callback(shadowhook_dl_info_t pre, shadowhook_dl_info_t post, void *data) {
    (void)pre; (void)post; (void)data;
    return 0;
}

int shadowhook_register_dl_fini_callback(shadowhook_dl_info_t pre, shadowhook_dl_info_t post, void *data) {
    (void)pre; (void)post; (void)data;
    return 0;
}

int shadowhook_unregister_dl_fini_callback(shadowhook_dl_info_t pre, shadowhook_dl_info_t post, void *data) {
    (void)pre; (void)post; (void)data;
    return 0;
}

void *shadowhook_get_prev_func(void *func) {
    return func;
}

void shadowhook_pop_stack(void *return_address) {
    (void)return_address;
}

void shadowhook_allow_reentrant(void *return_address) {
    (void)return_address;
}

void shadowhook_disallow_reentrant(void *return_address) {
    (void)return_address;
}

void *shadowhook_get_return_address(void) {
    return NULL;
}
