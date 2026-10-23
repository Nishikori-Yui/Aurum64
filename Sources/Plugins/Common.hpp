// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#define M64P_PLUGIN_PROTOTYPES
#include "m64p_common.h"
#include "m64p_plugin.h"
#include "AurumRuntime.h"
#define AUR_PLUGIN(TYPE, API, NAME) \
extern "C" EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type* t, int* v, int* a, const char** n, int* c) { \
    if(t) *t = TYPE; if(v) *v = 0x000100; if(a) *a = API; if(n) *n = NAME; if(c) *c = 0; return M64ERR_SUCCESS; } \
extern "C" EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle, void*, void (*)(void*,int,const char*)) { return M64ERR_SUCCESS; } \
extern "C" EXPORT m64p_error CALL PluginShutdown() { return M64ERR_SUCCESS; }
