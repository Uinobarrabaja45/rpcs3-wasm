#pragma once

#include <type_traits>

namespace utils
{
	// Hack. Pointer cast util to workaround UB. Use with extreme care.
	template <typename T, typename U> requires (std::is_pointer_v<std::remove_reference_t<U>>)
	[[nodiscard]] inline T* bless(const U& ptr)
	{
#ifdef _MSC_VER
		return (T*)ptr;
#elif defined(ARCH_X64)
		T* result;
		__asm__("movq %1, %0;" : "=r" (result) : "r" (ptr) : "memory");
		return result;
#elif defined(ARCH_ARM64)
		T* result;
		__asm__("mov %0, %1" : "=r" (result) : "r" (ptr) : "memory");
		return result;
#elif defined(__EMSCRIPTEN__) || defined(__wasm__)
		// wasm has no inline asm "register move" trick to defeat aliasing
		// analysis; the original purpose of bless() (workaround for the
		// strict-aliasing UB the type-pun would otherwise hit) is moot
		// on wasm where memory is a single linear array and the compiler
		// is much more conservative about reordering loads/stores anyway.
		return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(ptr));
#else
#error "Missing utils::bless() implementation"
#endif
	}
}

