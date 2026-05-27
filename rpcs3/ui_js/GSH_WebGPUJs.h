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

	// Upload a reference image to GPU, used as the "rendered output" until
	// the real RSX → WebGPU translator lands. Pixel data is tightly packed
	// RGBA8 (width*4 bytes per row); the caller may pad to a 256-byte
	// boundary if it wants to skip the staging copy. Stored as a
	// wgpu::Texture and drawn as a fullscreen triangle on every Render().
	void UploadReferenceTexture(const uint8_t* rgba, int width, int height);

private:
#ifdef HAS_WEBGPU
	void EnsureQuadPipeline();
	void DrawReferenceQuad(wgpu::RenderPassEncoder& pass);

	wgpu::Device m_device;
	wgpu::Queue m_queue;
	wgpu::Instance m_instance;
	wgpu::Surface m_surface;

	wgpu::RenderPipeline m_quadPipeline;
	wgpu::BindGroupLayout m_quadBindGroupLayout;
	wgpu::Sampler m_quadSampler;
	wgpu::Texture m_refTexture;
	wgpu::TextureView m_refTextureView;
	wgpu::BindGroup m_refBindGroup;
	int m_refTextureWidth = 0;
	int m_refTextureHeight = 0;
#endif
	std::string m_backend;
	int m_windowWidth = 1280;
	int m_windowHeight = 720;
	int m_framesRendered = 0;
	bool m_booted = false;
};
