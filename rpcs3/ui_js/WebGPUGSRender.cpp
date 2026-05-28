#include "stdafx.h"
#include "WebGPUGSRender.h"

#include <atomic>
#include <cstdio>

// Cross-thread flip counter — bumped on the RSX worker, read on the
// browser main thread inside GSH_WebGPUJs::Render and mainLoopTick.
// std::atomic<u64> is wait-free for load/store on every wasm32 backend
// we target, so the read path adds no contention.
std::atomic<u64> g_webgpu_flip_count{0};

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

void WebGPUGSRender::flip(const rsx::display_flip_info_t& /*info*/)
{
	const u64 n = g_webgpu_flip_count.fetch_add(1, std::memory_order_relaxed) + 1;
	if ((n % 60) == 1)
	{
		printf("[WebGPUGSRender] flip n=%llu\n", static_cast<unsigned long long>(n));
	}
	// No buffer present yet — that's Phase 7+ work: read the framebuffer
	// from guest VRAM (info.buffer or info.skip_frame guards), upload it
	// via wgpu::Queue::WriteTexture to a GSH_WebGPUJs-owned texture, and
	// have Render() sample that texture as the fullscreen quad.
}
