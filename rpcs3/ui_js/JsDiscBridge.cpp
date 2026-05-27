#include "JsDiscBridge.h"

#include <cstdio>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#include <unistd.h>
#endif

// JS-disc-device bridge implementation. See header for the threading /
// lifetime contract.
//
// All three calls funnel through MAIN_THREAD_EM_ASM (the *_INT variant
// for returns). That macro:
//   - on the main thread, runs the JS body inline;
//   - on a worker, calls `emscripten_sync_run_in_main_runtime_thread`
//     which serialises onto the DOM thread and blocks the worker.
// Either way, the JS code observes the same `Module.discImageDevice`
// the React shell created.
//
// The mutex around ReadAt is the only synchronisation we need: GetSize
// and HasFile are idempotent reads of JS properties, no state on the
// C++ side, no flag to race.

uint64_t JsDiscBridge::ReadAt(uint64_t offset, void* dst, uint64_t size)
{
	if (!dst || size == 0)
	{
		return 0;
	}

#ifdef __EMSCRIPTEN__
	std::lock_guard<std::mutex> lock(m_callMutex);

	// JS Number can losslessly hold 53-bit integers — PS3 BD-ROM caps
	// at ~50 GB, so splitting offset into low/high 32-bit halves keeps
	// things round-trippable on every browser even if it ever exceeded
	// 2^53. The JS side reassembles via `posLow + posHigh * 2^32`.
	const uint32_t posLow  = static_cast<uint32_t>(offset & 0xFFFFFFFFu);
	const uint32_t posHigh = static_cast<uint32_t>(offset >> 32);
	const uint32_t szLow   = static_cast<uint32_t>(size & 0xFFFFFFFFu);

	const int hasDevice = MAIN_THREAD_EM_ASM_INT({
		return (Module.discImageDevice && Module.discImageDevice.file) ? 1 : 0;
	});
	if (!hasDevice)
	{
		printf("[JsDiscBridge] ReadAt: no discImageDevice / no file attached\n");
		return 0;
	}

	MAIN_THREAD_EM_ASM({
		try {
			Module.discImageDevice.read($0, $1 + $2 * 4294967296, $3);
		} catch (e) {
			console.error('[JsDiscBridge] read threw:', e);
			Module.discImageDevice.doneFlag = true;
		}
	}, reinterpret_cast<uintptr_t>(dst), posLow, posHigh, szLow);

	// Poll the JS doneFlag until the slice + memcpy completes. 100 µs is
	// the same cadence Play-GamePlaytoo uses — small enough that an SSD
	// slice usually resolves in one or two iterations, large enough that
	// we don't burn the main thread on round-trips when waiting for
	// disk-bound reads.
	while (!MAIN_THREAD_EM_ASM_INT({ return Module.discImageDevice.isDone() ? 1 : 0; }))
	{
		usleep(100);
	}

	return size;
#else
	(void)offset;
	return 0;
#endif
}

uint64_t JsDiscBridge::GetSize()
{
#ifdef __EMSCRIPTEN__
	// Returning a double here because Emscripten's EM_ASM_INT path is
	// 32-bit; doubles round-trip through MAIN_THREAD_EM_ASM_DOUBLE
	// without losing the upper 21 bits.
	const double sz = MAIN_THREAD_EM_ASM_DOUBLE({
		if (!Module.discImageDevice || !Module.discImageDevice.file) return 0;
		return Module.discImageDevice.getFileSize();
	});
	if (sz < 0) return 0;
	return static_cast<uint64_t>(sz);
#else
	return 0;
#endif
}

bool JsDiscBridge::HasFile()
{
#ifdef __EMSCRIPTEN__
	return MAIN_THREAD_EM_ASM_INT({
		return (Module.discImageDevice && Module.discImageDevice.file) ? 1 : 0;
	}) != 0;
#else
	return false;
#endif
}
