#pragma once

#include "util/types.hpp"

#ifdef _M_X64
#ifdef _MSC_VER
extern "C" u64 __rdtsc();
#else
#include <immintrin.h>
#endif
#endif

#if defined(__EMSCRIPTEN__) || defined(__wasm__)
extern "C" double emscripten_get_now();
#endif

namespace utils
{
	inline u64 get_tsc()
	{
#if defined(ARCH_ARM64)
		u64 r = 0;
		__asm__ volatile("mrs %0, cntvct_el0" : "=r" (r));
		return r;
#elif defined(_M_X64)
		return __rdtsc();
#elif defined(ARCH_X64)
		return __builtin_ia32_rdtsc();
#elif defined(__EMSCRIPTEN__) || defined(__wasm__)
		// wasm has no rdtsc. Use the JS-side high-resolution clock; we
		// fake a TSC-like monotonic counter at nominal 3 GHz so consumers
		// that scale into wall-clock time see the right magnitude.
		// performance.now() is in milliseconds → * 3_000_000 for cycles.
		return static_cast<u64>(::emscripten_get_now() * 3'000'000.0);
#else
#error "Missing utils::get_tsc() implementation"
#endif
	}
}
