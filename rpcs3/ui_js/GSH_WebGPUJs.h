#pragma once

#include <string>
#include <vector>
#include <cstdint>

#ifdef HAS_WEBGPU
#include <webgpu/webgpu_cpp.h>
#endif

// WebGPU-backed RSX (PS3 GPU) graphics handler for the browser build.
//
// In native rpcs3, the RSX backend is selected at runtime — see
// VKGSRender.cpp / GLGSRender.cpp under Emu/RSX/{VK,GL}. The WebGPU
// equivalent does roughly what VKGSRender does, but routes everything
// through wgpu::* instead of vk::*:
//
//   - command_buffer  → wgpu::CommandEncoder
//   - render_pass     → wgpu::RenderPassEncoder
//   - pipeline        → wgpu::RenderPipeline (WGSL-compiled fragment
//                       and vertex shaders translated from RSX FP/VP)
//   - texture         → wgpu::Texture
//   - vertex/index    → wgpu::Buffer (USAGE_VERTEX | USAGE_INDEX)
//   - VRAM uploads    → device.queue.writeBuffer / writeTexture
//
// The translator that lowers RSX shader programs (NV47-derived) to WGSL is
// the largest single chunk of work; this header just declares the surface.
class CGSH_WebGPUJs
{
public:
#ifdef HAS_WEBGPU
	CGSH_WebGPUJs(wgpu::Device device, wgpu::Instance instance, wgpu::Surface surface, const std::string& backend);
#else
	CGSH_WebGPUJs(const std::string& backend);
#endif
	~CGSH_WebGPUJs();

	void Initialize();
	void Release();

	void SetPresentationParams(int windowWidth, int windowHeight);
	void PresentBackbuffer();
	void ConfigureSurface();

	// Render one frame. Called from the Emscripten main loop in Main.cpp
	// at the browser's `requestAnimationFrame` rate. In the full integration
	// build this drives the RSX-derived render passes; here it draws an
	// animated clear color so the user can see WebGPU is actually wired up
	// to the canvas. Returns `true` if a frame was presented.
	bool Render();

	// Whether a boot has been requested and the renderer should start
	// producing real frames (animated clear color rather than the
	// "splash" color shown during initialization).
	void SetBooted(bool booted) { m_booted = booted; }

private:
#ifdef HAS_WEBGPU
	wgpu::Device m_device;
	wgpu::Queue m_queue;
	wgpu::Instance m_instance;
	wgpu::Surface m_surface;
	// Lazily-created texture holding the most recent PS3 front buffer,
	// uploaded from guest VRAM and copied to the swapchain each frame.
	wgpu::Texture m_fbTexture;
	uint32_t m_fbTexW = 0;
	uint32_t m_fbTexH = 0;
	uint64_t m_lastFlip = 0;          // last g_webgpu_flip_count we uploaded
	uint64_t m_lastDrawnFlip = ~0ull; // last flip count we actually presented (idle throttle)
	std::vector<uint32_t> m_fbStaging; // CPU scratch for the ARGB→BGRA swizzle
#endif
	std::string m_backend;
	int m_windowWidth = 1280;
	int m_windowHeight = 720;
	int m_framesRendered = 0;
	bool m_booted = false;
};
