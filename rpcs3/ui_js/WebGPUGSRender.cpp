#include "stdafx.h"
#include "WebGPUGSRender.h"
#include "Emu/Memory/vm.h"
#include "Emu/RSX/gcm_enums.h"

#include <atomic>
#include <cstdio>

// Cross-thread flip counter — bumped on the RSX worker, read on the
// browser main thread inside GSH_WebGPUJs::Render and mainLoopTick.
// std::atomic<u64> is wait-free for load/store on every wasm32 backend
// we target, so the read path adds no contention.
std::atomic<u64> g_webgpu_flip_count{0};

// Front display-buffer descriptor published by flip() (RSX worker) and read
// by GSH_WebGPUJs::Render (browser main thread). wasm32 pthreads share one
// linear memory, so the main thread can read the framebuffer pixels directly
// from vm::base(g_webgpu_fb_addr). These are plain scalars guarded by the
// monotonic g_webgpu_flip_count: Render only re-reads VRAM when the count
// changes, so a torn descriptor is at worst one stale frame (harmless).
std::atomic<u32> g_webgpu_fb_addr{0};
std::atomic<u32> g_webgpu_fb_width{0};
std::atomic<u32> g_webgpu_fb_height{0};
std::atomic<u32> g_webgpu_fb_pitch{0};
// Host pointer (vm::base(addr)) of the front buffer, resolved here on the RSX
// worker so GSH_WebGPUJs.cpp can read the pixels without pulling in the rpcs3
// vm/types headers. Valid on the browser main thread too — wasm pthreads
// share one address space. 0 == no frame yet.
std::atomic<u64> g_webgpu_fb_host{0};

u64 WebGPUGSRender::get_cycles()
{
	return thread_ctrl::get_cycles(static_cast<named_thread<WebGPUGSRender>&>(*this));
}

WebGPUGSRender::WebGPUGSRender(utils::serial* ar) noexcept : GSRender(ar)
{
	printf("[WebGPUGSRender] constructed\n");
}

void WebGPUGSRender::end()
{
	// Same body as NullGSRender — no actual draw. Phase 7+ will replace
	// this with WGSL pipeline dispatch translated from RSX FP/VP.
	execute_nop_draw();
	rsx::thread::end();
}

void WebGPUGSRender::flip(const rsx::display_flip_info_t& info)
{
	// Publish the front display-buffer descriptor BEFORE bumping the flip
	// count, so a Render() that observes the new count always sees a matching
	// (addr,w,h,pitch). The store order + the count fence give the main thread
	// a consistent view without a lock.
	if (info.buffer < display_buffers_count)
	{
		const auto& db = display_buffers[info.buffer];
		// Extract the be_t<u32> fields to plain u32 first (a direct ternary on
		// be_t<> is ambiguous), then publish.
		const u32 off       = db.offset;
		const u32 w         = db.width;
		const u32 h         = db.height;
		const u32 raw_pitch = db.pitch;
		const u32 pitch = raw_pitch ? raw_pitch : w * 4;
		const u32 addr  = rsx::get_address(off, CELL_GCM_LOCATION_LOCAL);
		const u64 host  = addr ? reinterpret_cast<uintptr_t>(vm::base(addr)) : 0;
		g_webgpu_fb_pitch.store(pitch, std::memory_order_relaxed);
		g_webgpu_fb_width.store(w, std::memory_order_relaxed);
		g_webgpu_fb_height.store(h, std::memory_order_relaxed);
		g_webgpu_fb_host.store(host, std::memory_order_relaxed);
		g_webgpu_fb_addr.store(addr, std::memory_order_release);
	}

	const u64 n = g_webgpu_flip_count.fetch_add(1, std::memory_order_release) + 1;

	// PHASE-7 PROBE: does the display buffer in guest memory actually hold
	// pixels? Our draw path is execute_nop_draw, so the GPU never writes a
	// surface. If these bytes are real image data, the game (or RSX 2D
	// blit engine) populated them on the CPU side and we can blit them to
	// the swapchain cheaply. If they're zero/uniform, we need a real
	// RSX→WebGPU draw translator. Log the first few flips + periodically.
	if (n <= 4 || (n % 300) == 1)
	{
		printf("[WebGPUGSRender] flip n=%llu buf=%u count=%u\n",
			static_cast<unsigned long long>(n), info.buffer, display_buffers_count);

		if (info.buffer < display_buffers_count)
		{
			const auto& db = display_buffers[info.buffer];
			const u32 off       = db.offset;
			const u32 w         = db.width;
			const u32 h         = db.height;
			const u32 raw_pitch = db.pitch;
			const u32 pitch = raw_pitch ? raw_pitch : w * 4;
			const u32 addr  = rsx::get_address(off, CELL_GCM_LOCATION_LOCAL);
			printf("[WebGPUGSRender]   db[%u] off=0x%x %ux%u pitch=%u vm_addr=0x%x\n",
				info.buffer, off, w, h, pitch, addr);

			if (addr && w && h)
			{
				const u8* base = static_cast<const u8*>(vm::base(addr));
				// Sample the center row: count non-black and distinct pixels.
				const u32 cy = h / 2;
				const u8* row = base + static_cast<u64>(cy) * pitch;
				u32 nonblack = 0, distinct = 0, prev = 0xdeadbeef;
				for (u32 i = 0; i < w && i < 256; i++)
				{
					const u32 px = *reinterpret_cast<const u32*>(row + static_cast<u64>(i) * 4);
					if (px & 0x00ffffffu) nonblack++;
					if (px != prev) { distinct++; prev = px; }
				}
				printf("[WebGPUGSRender]   centerRow nonblack=%u distinct=%u px[0..3]=[%08x %08x %08x %08x]\n",
					nonblack, distinct,
					*reinterpret_cast<const u32*>(row + 0),
					*reinterpret_cast<const u32*>(row + 4),
					*reinterpret_cast<const u32*>(row + 8),
					*reinterpret_cast<const u32*>(row + 12));
			}
		}
	}

	// No buffer present yet — that's Phase 7+ work: read the framebuffer
	// from guest VRAM (info.buffer or info.skip_frame guards), upload it
	// via wgpu::Queue::WriteTexture to a GSH_WebGPUJs-owned texture, and
	// have Render() sample that texture as the fullscreen quad.
}
