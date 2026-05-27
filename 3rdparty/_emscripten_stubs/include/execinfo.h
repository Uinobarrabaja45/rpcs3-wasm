// Emscripten/WASM stub for <execinfo.h>. Emscripten doesn't ship
// execinfo; rpcs3 uses backtrace() only for crash diagnostics, which
// the browser can't usefully surface anyway. Bodies in execinfo_stub.cpp.

#pragma once

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

int   backtrace(void** buffer, int size);
char** backtrace_symbols(void* const* buffer, int size);

#ifdef __cplusplus
}
#endif
