#pragma once
#include "Emu/RSX/GSRender.h"

// rsx::thread subclass for the WASM/WebGPU port.
//
// Architecturally mirrors NullGSRender — inherits the base rsx::thread
// FIFO pump (which decodes cellGcm commands out of guest memory and
// advances the FIFO pointer) and stubs out the draw path. The reason
// we don't just keep using NullGSRender is that we need a place to
// observe RSX-side events (flips, in particular) from JS:
//
//   - The browser's animation frame loop in Main.cpp (mainLoopTick)
//     drives wgpu render-pass submission at requestAnimationFrame rate.
//   - The PS3 game requests a buffer flip whenever it's done drawing,
//     via cellGcmSetFlip → rsx::thread::flip(). For the WebGPU surface
//     to actually present *the right* frame, mainLoopTick needs to know
//     when a flip has been requested.
//
// This subclass records `g_webgpu_flip_count` per flip(); GSH_WebGPUJs
// reads it to vary the clear color (sanity check that flips reach the
// browser side) and, in Phase 7+, to pick up the surface texture from
// guest VRAM and forward it via wgpu queue.writeTexture.
//
// `end()` mirrors NullGSRender — `execute_nop_draw()` + `rsx::thread::end()`.
// Until the RSX → WGSL translator lands under Emu/RSX/WebGPU/, this is
// still a no-draw backend; the only WebGPU output is the animated clear
// in GSH_WebGPUJs::Render. The point of the subclass is purely the flip
// observation hook above.
class WebGPUGSRender : public GSRender
{
public:
	u64 get_cycles() final;

	WebGPUGSRender(utils::serial* ar) noexcept;
	WebGPUGSRender() noexcept : WebGPUGSRender(nullptr) {}

private:
	void end() override;
	void flip(const rsx::display_flip_info_t& info) override;
	void clear_surface(u32 arg) override;
};

// Visible to GSH_WebGPUJs / Main.cpp — read-only from the browser side.
// Bumped by the RSX thread on every flip(); never reset (monotonic).
extern std::atomic<u64> g_webgpu_flip_count;

// Front display-buffer descriptor, published by flip() each frame. The
// browser main thread reads the pixels straight out of guest VRAM via
// vm::base(g_webgpu_fb_addr) (shared wasm linear memory) and blits them to
// the WebGPU surface. addr==0 means "no real frame yet" → present black.
extern std::atomic<u32> g_webgpu_fb_addr;
extern std::atomic<u32> g_webgpu_fb_width;
extern std::atomic<u32> g_webgpu_fb_height;
extern std::atomic<u32> g_webgpu_fb_pitch;
extern std::atomic<u64> g_webgpu_fb_host; // host ptr of front buffer (wasm shared mem)
