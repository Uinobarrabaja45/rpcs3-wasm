#include "GSH_WebGPUJs.h"

#include <cstdio>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <vector>

#ifdef HAS_WEBGPU
#include <emscripten/heap.h>
// Front display-buffer descriptor published by WebGPUGSRender::flip() on the
// RSX worker. Declared here (rather than including WebGPUGSRender.h, which
// drags in the whole RSX GSRender base + rpcs3 vm/types headers) since we only
// need the scalars. g_webgpu_fb_host is already a host pointer (resolved via
// vm::base on the RSX side), valid on this thread because wasm pthreads share
// one linear address space.
extern std::atomic<uint64_t> g_webgpu_flip_count;
extern std::atomic<uint32_t> g_webgpu_fb_addr;
extern std::atomic<uint32_t> g_webgpu_fb_width;
extern std::atomic<uint32_t> g_webgpu_fb_height;
extern std::atomic<uint32_t> g_webgpu_fb_pitch;
extern std::atomic<uint64_t> g_webgpu_fb_host;
#endif

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
	// Idle-frame throttle: the canvas only changes when the guest flips a new
	// front buffer. Re-submitting the same image at requestAnimationFrame rate
	// (~120fps) is pure waste — and empirically the headless WebGPU renderer
	// destabilizes after a few minutes of that idle churn (the flaky ~3.5-7.5min
	// "Debugger connection closed", which is NOT OOM — the wasm heap stays flat).
	// Skip the WebGPU work unless: the first frame, a new flip arrived, or a slow
	// keep-alive tick. This ~100x fewer submissions lets the slow (finite) boot
	// run long enough to reach its first real flip → the 505 splash.
	{
		const uint64_t flipNow = g_webgpu_flip_count.load(std::memory_order_acquire);
		const bool firstFrame  = (m_framesRendered == 0);
		const bool flipped     = (flipNow != m_lastDrawnFlip);
		const bool keepAlive   = (m_framesRendered % 120) == 0;
		if (!firstFrame && !flipped && !keepAlive)
		{
			m_framesRendered++;
			return true;
		}
		m_lastDrawnFlip = flipNow;
	}

	// older swap-chain API; surface.GetCurrentTexture() is the path.
	wgpu::SurfaceTexture surfaceTex = {};
	m_surface.GetCurrentTexture(&surfaceTex);
	if (!surfaceTex.texture)
	{
		// Surface lost (window resize, device-recreate) — caller will
		// re-configure on the next frame.
		return false;
	}

	// Latest PS3 front-buffer descriptor published by WebGPUGSRender::flip().
	// acquire-load the count first, then the addr, mirroring flip()'s release
	// stores so a non-zero addr is always paired with valid w/h/pitch.
	const uint64_t flip     = g_webgpu_flip_count.load(std::memory_order_acquire);
	const uint32_t fb_addr  = g_webgpu_fb_addr.load(std::memory_order_acquire);
	const uint32_t fb_w     = g_webgpu_fb_width.load(std::memory_order_relaxed);
	const uint32_t fb_h     = g_webgpu_fb_height.load(std::memory_order_relaxed);
	const uint32_t fb_pitch = g_webgpu_fb_pitch.load(std::memory_order_relaxed);

	if (fb_w > 0 && fb_h > 0 && (static_cast<int>(fb_w) != m_windowWidth || static_cast<int>(fb_h) != m_windowHeight))
	{
		m_windowWidth = fb_w;
		m_windowHeight = fb_h;
		ConfigureSurface();
	}

	const uint32_t surfW = static_cast<uint32_t>(m_windowWidth);
	const uint32_t surfH = static_cast<uint32_t>(m_windowHeight);

	// We can present honest pixels once the game flips a buffer.
	// Since we update the surface resolution on size changes, a 1:1 copy is valid.
	const bool haveFrame = fb_addr && fb_w == surfW && fb_h == surfH && fb_pitch >= fb_w * 4;

	wgpu::CommandEncoderDescriptor encDesc = {};
	wgpu::CommandEncoder encoder = m_device.CreateCommandEncoder(&encDesc);
	if (!encoder) return false;

	bool blitted = false;
	if (haveFrame)
	{
		if (!m_fbTexture || m_fbTexW != fb_w || m_fbTexH != fb_h)
		{
			wgpu::TextureDescriptor td = {};
			td.usage         = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc;
			td.dimension     = wgpu::TextureDimension::e2D;
			td.size          = { fb_w, fb_h, 1 };
			td.format        = wgpu::TextureFormat::BGRA8Unorm;
			td.mipLevelCount = 1;
			td.sampleCount   = 1;
			m_fbTexture = m_device.CreateTexture(&td);
			m_fbTexW = fb_w;
			m_fbTexH = fb_h;
			m_lastFlip = 0; // force a re-upload into the new texture
		}

		// Re-read VRAM only when a new frame was flipped (the splash is static,
		// so this is one upload, not one per browser frame).
		if (m_fbTexture && flip != m_lastFlip)
		{
			const uint8_t* base = reinterpret_cast<const uint8_t*>(
				static_cast<uintptr_t>(g_webgpu_fb_host.load(std::memory_order_relaxed)));
			if (base)
			{
				m_fbStaging.resize(static_cast<size_t>(fb_w) * fb_h);
				// PS3 display buffers are A8R8G8B8 big-endian: memory bytes are
				// [A][R][G][B]. Packing them as a host little-endian u32 lays the
				// bytes out [B][G][R][A] == BGRA8Unorm, which is the surface
				// format — so the swizzle is just this byte repack.
				for (uint32_t y = 0; y < fb_h; y++)
				{
					const uint8_t* srcRow = base + static_cast<size_t>(y) * fb_pitch;
					uint32_t* dstRow = m_fbStaging.data() + static_cast<size_t>(y) * fb_w;
					for (uint32_t x = 0; x < fb_w; x++)
					{
						const uint8_t* p = srcRow + static_cast<size_t>(x) * 4;
						dstRow[x] = (static_cast<uint32_t>(p[0]) << 24) |
						            (static_cast<uint32_t>(p[1]) << 16) |
						            (static_cast<uint32_t>(p[2]) << 8)  |
						             static_cast<uint32_t>(p[3]);
					}
				}

				wgpu::TexelCopyTextureInfo dst = {};
				dst.texture = m_fbTexture;
				dst.aspect  = wgpu::TextureAspect::All;
				wgpu::TexelCopyBufferLayout layout = {};
				layout.bytesPerRow  = fb_w * 4;
				layout.rowsPerImage = fb_h;
				wgpu::Extent3D ext = { fb_w, fb_h, 1 };
				m_queue.WriteTexture(&dst, m_fbStaging.data(),
					static_cast<size_t>(fb_w) * fb_h * 4, &layout, &ext);
				m_lastFlip = flip;
			}
		}

		if (m_fbTexture)
		{
			wgpu::TexelCopyTextureInfo src = {};
			src.texture = m_fbTexture;
			src.aspect  = wgpu::TextureAspect::All;
			wgpu::TexelCopyTextureInfo dstC = {};
			dstC.texture = surfaceTex.texture;
			dstC.aspect  = wgpu::TextureAspect::All;
			wgpu::Extent3D copyExt = { fb_w, fb_h, 1 };
			encoder.CopyTextureToTexture(&src, &dstC, &copyExt);
			blitted = true;
		}
	}

	if (!blitted)
	{
		// No real frame yet — present solid black (honest "nothing rendered").
		// This deliberately replaces the old animated placeholder clear.
		wgpu::TextureView view = surfaceTex.texture.CreateView();
		if (!view) return false;
		wgpu::RenderPassColorAttachment ca = {};
		ca.view       = view;
		ca.loadOp     = wgpu::LoadOp::Clear;
		ca.storeOp    = wgpu::StoreOp::Store;
		ca.clearValue = { 0.0, 0.0, 0.0, 1.0 };
		ca.depthSlice = wgpu::kDepthSliceUndefined;
		wgpu::RenderPassDescriptor pd = {};
		pd.colorAttachmentCount = 1;
		pd.colorAttachments     = &ca;
		wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pd);
		pass.End();
	}

	wgpu::CommandBuffer cmd = encoder.Finish();
	m_queue.Submit(1, &cmd);

	m_framesRendered++;
	{
		// Event-driven log: fire on ANY change of fb/flip/blit (so the first real
		// framebuffer/flip is caught instantly), plus a sparse 1200-frame (~10s)
		// heartbeat. This stops the per-second flood from rolling the SCALL/SEMBLOCK
		// stall-trace out of the console ring buffer.
		static thread_local unsigned s_pfb = 0xFFFFFFFFu; static thread_local unsigned long long s_pflip = ~0ull; static thread_local int s_pblit = -1;
		const bool changed = (fb_addr != s_pfb) || (static_cast<unsigned long long>(flip) != s_pflip) || ((blitted ? 1 : 0) != s_pblit);
		if (changed || (m_framesRendered % 1200) == 1)
		{
			const double heapMiB = static_cast<double>(emscripten_get_heap_size()) / (1024.0 * 1024.0);
			printf("[GSH_WebGPUJs] Render frame=%d booted=%d blit=%d fb=0x%x %ux%u flip=%llu heap=%.0fMiB%s\n",
			       m_framesRendered, m_booted ? 1 : 0, blitted ? 1 : 0,
			       fb_addr, fb_w, fb_h, static_cast<unsigned long long>(flip), heapMiB, changed ? " <CHANGED>" : "");
			s_pfb = fb_addr; s_pflip = static_cast<unsigned long long>(flip); s_pblit = blitted ? 1 : 0;
		}
	}
	return true;
#else
	return false;
#endif
}
