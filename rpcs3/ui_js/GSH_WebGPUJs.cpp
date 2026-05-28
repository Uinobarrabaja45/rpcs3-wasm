#include "GSH_WebGPUJs.h"

#include <cstdio>
#include <cmath>

// Minimal WebGPU render path — see GSH_WebGPUJs.h for the integration
// plan. The real RSX-equivalent backend lives under Emu/RSX/WebGPU/ once
// it's written (plan-poc-ps3-webgpu.md §2.2). Until then `Render()` does
// the smallest thing that proves the wgpu::Surface → HTMLCanvas pipeline
// works end-to-end: acquire the next surface texture, encode a single
// render pass that clears to an animated color, then submits + presents.
//
// Why an animated clear:
//   - A static clear can be confused with a hung tab or a cached frame.
//   - The animation runs off m_framesRendered, so a screenshot at any
//     point shows a different color — proves the render loop is alive.
//   - When the rest of the pipeline lands (RSX → WGSL translator), this
//     becomes the "no-game-booted" splash; the if(m_booted) branch will
//     swap in real RSX command playback.

#ifdef HAS_WEBGPU

CGSH_WebGPUJs::CGSH_WebGPUJs(wgpu::Device device, wgpu::Instance instance, wgpu::Surface surface, const std::string& backend)
    : m_device(device)
    , m_instance(instance)
    , m_surface(surface)
    , m_backend(backend)
{
	printf("[GSH_WebGPUJs] constructed with backend='%s'\n", backend.c_str());
	m_queue = m_device.GetQueue();
}
#else
CGSH_WebGPUJs::CGSH_WebGPUJs(const std::string& backend)
    : m_backend(backend)
{
	printf("[GSH_WebGPUJs] constructed (no-WebGPU build) with backend='%s'\n", backend.c_str());
}
#endif

CGSH_WebGPUJs::~CGSH_WebGPUJs()
{
	Release();
}

void CGSH_WebGPUJs::Initialize()
{
	printf("[GSH_WebGPUJs] Initialize — surface configured %dx%d\n", m_windowWidth, m_windowHeight);
	ConfigureSurface();
}

void CGSH_WebGPUJs::Release()
{
	printf("[GSH_WebGPUJs] Release\n");
}

void CGSH_WebGPUJs::SetPresentationParams(int windowWidth, int windowHeight)
{
	m_windowWidth = windowWidth;
	m_windowHeight = windowHeight;
	printf("[GSH_WebGPUJs] SetPresentationParams %dx%d\n", windowWidth, windowHeight);
}

void CGSH_WebGPUJs::PresentBackbuffer()
{
	// Legacy stub kept for API symmetry with Play!'s CGSH_WebGPUJs.
	// New code paths through Render() instead, which combines the
	// command-buffer build + present in one pass.
}

void CGSH_WebGPUJs::ConfigureSurface()
{
#ifdef HAS_WEBGPU
	if (!m_surface || !m_device) return;
	wgpu::SurfaceConfiguration cfg = {};
	cfg.device      = m_device;
	cfg.format      = wgpu::TextureFormat::BGRA8Unorm;
	cfg.usage       = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopyDst;
	cfg.alphaMode   = wgpu::CompositeAlphaMode::Auto;
	cfg.width       = static_cast<uint32_t>(m_windowWidth);
	cfg.height      = static_cast<uint32_t>(m_windowHeight);
	cfg.presentMode = wgpu::PresentMode::Fifo;
	m_surface.Configure(&cfg);
	printf("[GSH_WebGPUJs] ConfigureSurface %dx%d BGRA8Unorm Fifo\n", m_windowWidth, m_windowHeight);
#endif
}

bool CGSH_WebGPUJs::Render()
{
#ifdef HAS_WEBGPU
	if (!m_surface || !m_device || !m_queue) return false;

	// Acquire next swap-chain texture. emdawnwebgpu doesn't expose the
	// older swap-chain API; surface.GetCurrentTexture() is the path.
	wgpu::SurfaceTexture surfaceTex = {};
	m_surface.GetCurrentTexture(&surfaceTex);
	if (!surfaceTex.texture)
	{
		// Surface lost (window resize, device-recreate) — caller will
		// re-configure on the next frame.
		return false;
	}

	wgpu::TextureView view = surfaceTex.texture.CreateView();
	if (!view) return false;

	// Animated clear color — the smallest thing that proves the render
	// loop is alive. RSX-derived geometry replaces this once the
	// translator under Emu/RSX/WebGPU/ lands.
	const float t = static_cast<float>(m_framesRendered) * 0.01f;
	wgpu::Color clearColor;
	if (m_booted)
	{
		// Warm gradient — orange/red cycling — game is "running".
		clearColor.r = 0.55 + 0.25 * std::sin(t * 1.3);
		clearColor.g = 0.20 + 0.10 * std::sin(t * 0.7 + 1.0);
		clearColor.b = 0.05 + 0.05 * std::sin(t * 0.5 + 2.0);
		clearColor.a = 1.0;
	}
	else
	{
		// Cool slate — pre-boot splash.
		clearColor.r = 0.05 + 0.05 * std::sin(t);
		clearColor.g = 0.10 + 0.05 * std::sin(t + 1.0);
		clearColor.b = 0.20 + 0.10 * std::sin(t + 2.0);
		clearColor.a = 1.0;
	}

	wgpu::RenderPassColorAttachment colorAttachment = {};
	colorAttachment.view       = view;
	colorAttachment.loadOp     = wgpu::LoadOp::Clear;
	colorAttachment.storeOp    = wgpu::StoreOp::Store;
	colorAttachment.clearValue = clearColor;
	colorAttachment.depthSlice = wgpu::kDepthSliceUndefined;

	wgpu::RenderPassDescriptor passDesc = {};
	passDesc.colorAttachmentCount = 1;
	passDesc.colorAttachments     = &colorAttachment;

	wgpu::CommandEncoderDescriptor encDesc = {};
	wgpu::CommandEncoder encoder = m_device.CreateCommandEncoder(&encDesc);
	if (!encoder) return false;

	wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDesc);
	// Phase 4+ will record RSX-derived geometry here.
	pass.End();

	wgpu::CommandBuffer cmd = encoder.Finish();
	m_queue.Submit(1, &cmd);

	// Note: in emdawnwebgpu the swap-chain is auto-presented when the
	// JS event loop returns — no explicit Surface.Present() needed.

	m_framesRendered++;
	if ((m_framesRendered % 60) == 1)
	{
		printf("[GSH_WebGPUJs] Render frame=%d booted=%d clear=(%.2f,%.2f,%.2f)\n",
		       m_framesRendered, m_booted ? 1 : 0,
		       static_cast<float>(clearColor.r),
		       static_cast<float>(clearColor.g),
		       static_cast<float>(clearColor.b));
	}
	return true;
#else
	return false;
#endif
}
