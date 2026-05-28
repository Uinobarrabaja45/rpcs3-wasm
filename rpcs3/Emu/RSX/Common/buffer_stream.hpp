#pragma once

#include "util/types.hpp"
#include <cstring>

#if defined(ARCH_X64)
#include "emmintrin.h"
#include "immintrin.h"
#endif

#ifdef ARCH_ARM64
#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include "Emu/CPU/sse2neon.h"
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif
#endif

namespace utils
{
#if defined(__EMSCRIPTEN__) || defined(__wasm__)
	// wasm32 has WASM SIMD (wasm_simd128.h) but rpcs3's calls here are
	// just "memcpy 16 bytes" — the SSE intrinsics' streaming-store hint
	// doesn't translate, so use plain memcpy. Vertex-buffer upload is
	// the only call site; performance hit is a few % at worst.

	static inline
		void stream_vector(void* dst, u32 x, u32 y, u32 z, u32 w)
	{
		const u32 v[4] = {x, y, z, w};
		std::memcpy(dst, v, sizeof(v));
	}

	static inline
		void stream_vector(void* dst, f32 x, f32 y, f32 z, f32 w)
	{
		stream_vector(dst, std::bit_cast<u32>(x), std::bit_cast<u32>(y), std::bit_cast<u32>(z), std::bit_cast<u32>(w));
	}

	template <int Count = 1>
	void stream_vector_from_memory(void* dst, void* src)
	{
		std::memcpy(dst, src, 16 * Count);
	}
#else
	/**
	 * Stream a 128 bits vector to dst.
	 */
	static inline
		void stream_vector(void* dst, u32 x, u32 y, u32 z, u32 w)
	{
		const __m128i vector = _mm_set_epi32(w, z, y, x);
		_mm_stream_si128(reinterpret_cast<__m128i*>(dst), vector);
	}

	static inline
		void stream_vector(void* dst, f32 x, f32 y, f32 z, f32 w)
	{
		stream_vector(dst, std::bit_cast<u32>(x), std::bit_cast<u32>(y), std::bit_cast<u32>(z), std::bit_cast<u32>(w));
	}

	/**
	 * Stream a 128 bits vector from src to dst.
	 */
	template <int Count = 1>
	void stream_vector_from_memory(void* dst, void* src)
	{
		auto _src = reinterpret_cast<__m128i*>(src);
		auto _dst = reinterpret_cast<__m128i*>(dst);
		for (int i = 0; i < Count; ++i, ++_src, ++_dst)
		{
			const __m128i vector = _mm_loadu_si128(_src);
			_mm_stream_si128(_dst, vector);
		}
	}
#endif
}
