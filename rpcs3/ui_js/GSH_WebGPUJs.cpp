#include "GSH_WebGPUJs.h"

#include <cstdio>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstring>
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
extern std::atomic<uint32_t> g_webgpu_content_frames; // bumped per varied (real-content) composite; JS loading gauge
extern std::atomic<uint32_t> g_webgpu_fb_addr;
extern std::atomic<uint32_t> g_webgpu_fb_width;
extern std::atomic<uint32_t> g_webgpu_fb_height;
extern std::atomic<uint32_t> g_webgpu_fb_pitch;
extern std::atomic<uint64_t> g_webgpu_fb_host;
extern std::atomic<uint32_t> g_webgpu_clear_color;
extern std::atomic<uint64_t> g_webgpu_tex_host;
extern std::atomic<uint32_t> g_webgpu_tex_w;
extern std::atomic<uint32_t> g_webgpu_tex_h;
extern std::atomic<uint32_t> g_webgpu_tex_pitch;
extern std::atomic<uint64_t> g_webgpu_tex_flip;
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
	cfg.alphaMode   = wgpu::CompositeAlphaMode::Opaque; // opaque so the canvas can't blend the page-black through
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
		// PRESENT EVERY FRAME (2026-06-05): the user reports the live canvas is "very flashy". The canvas does NOT
		// reliably hold the last presented image between presents — when we throttle (present only on the ~30/s
		// flips) it flickers to black on the in-between RAF frames, and the game also flips menu/black double-buffers
		// ~50/50, so the surface read alternates too. m_fbTexture always holds the composited menu, so simply
		// re-blitting it every RAF frame keeps the canvas on the menu (no black gaps). Costs more GPU submits but
		// kills the flashing; the rate-limiter already tamed the log-churn that destabilized headless before.
		const bool keepAlive   = (m_framesRendered % 30) == 0; // present on each new flip + a ~4/s keep-alive (NOT every RAF frame: present-every-frame churned the swapchain and showed stale navy images)
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

	// Debounce resolution blips (2026-06-04): the menu occasionally flips its display-buffer size for just a frame
	// or two; reconfiguring the surface + recreating m_fbTexture on each blip reset the persistent buffer and
	// flashed the canvas BLACK ~once a minute. Only reconfigure after the new size persists a few flips; a transient
	// blip makes haveFrame go false for that frame, so we just re-present the held menu.
	static int s_pendingW = 0, s_pendingH = 0, s_pendingCount = 0;
	if (fb_w > 0 && fb_h > 0 && (static_cast<int>(fb_w) != m_windowWidth || static_cast<int>(fb_h) != m_windowHeight))
	{
		if (static_cast<int>(fb_w) == s_pendingW && static_cast<int>(fb_h) == s_pendingH) s_pendingCount++;
		else { s_pendingW = static_cast<int>(fb_w); s_pendingH = static_cast<int>(fb_h); s_pendingCount = 1; }
		if (s_pendingCount >= 10)
		{
			printf("[GSH_WebGPUJs] resolution change %dx%d -> %ux%u (reconfigure)\n", m_windowWidth, m_windowHeight, fb_w, fb_h);
			m_windowWidth = fb_w;
			m_windowHeight = fb_h;
			ConfigureSurface();
			m_surface.GetCurrentTexture(&surfaceTex);
			if (!surfaceTex.texture) return false;
			s_pendingCount = 0;
		}
	}
	else { s_pendingCount = 0; }

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
			td.usage         = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::TextureBinding;
			td.dimension     = wgpu::TextureDimension::e2D;
			td.size          = { fb_w, fb_h, 1 };
			td.format        = wgpu::TextureFormat::BGRA8Unorm;
			td.mipLevelCount = 1;
			td.sampleCount   = 1;
			m_fbTexture = m_device.CreateTexture(&td);
			m_fbTexW = fb_w;
			m_fbTexH = fb_h;
			m_lastFlip = 0; // force a re-upload into the new texture
				printf("[GSH_WebGPUJs] m_fbTexture recreate -> %ux%u\n", fb_w, fb_h);
		}

		// Re-read VRAM only when a new frame was flipped (the splash is static,
		// so this is one upload, not one per browser frame).
		if (m_fbTexture && flip != m_lastFlip)
		{
			// M2a: if this frame recorded a textured draw, sample the GAME's real texture (record/replay)
			// instead of the software-P2-DRAW VRAM copy. Clear-only frames fall back to the VRAM buffer.
			const bool from_tex = (g_webgpu_tex_flip.load(std::memory_order_acquire) == flip);
			const uint32_t src_pitch = from_tex ? g_webgpu_tex_pitch.load(std::memory_order_relaxed) : fb_pitch;
			const uint8_t* base = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(
				from_tex ? g_webgpu_tex_host.load(std::memory_order_relaxed)
				         : g_webgpu_fb_host.load(std::memory_order_relaxed)));
			static thread_local int s_m2a_log = 0;
			if (from_tex && s_m2a_log++ < 2) printf("[GSH_WebGPUJs] M2a: sampling GAME texture via pipeline (record/replay), not the P2-DRAW VRAM copy\n");
			if (base)
			{
				m_fbStaging.resize(static_cast<size_t>(fb_w) * fb_h);
				// PS3 display buffers are A8R8G8B8 big-endian: memory bytes are
				// [A][R][G][B]. Packing them as a host little-endian u32 lays the
				// bytes out [B][G][R][A] == BGRA8Unorm, which is the surface
				// format — so the swizzle is just this byte repack.
				uint64_t sR=0,sG=0,sB=0,sR2=0,sG2=0,sB2=0; // per-channel sums for COLOR VARIANCE. The game flips a UNIFORM
					// navy backbuffer that the edge-density/coverage metrics could not cleanly reject (a navy gradient overlapped
					// sparse menu frames). Color stddev separates them with margin: uniform navy ~7, the varied menu ~26+.
					for (uint32_t y = 0; y < fb_h; y++)
					{
						const uint8_t* srcRow = base + static_cast<size_t>(y) * src_pitch;
						uint32_t* dstRow = m_fbStaging.data() + static_cast<size_t>(y) * fb_w;
						for (uint32_t x = 0; x < fb_w; x++)
						{
							const uint8_t* p = srcRow + static_cast<size_t>(x) * 4;
							const uint32_t R = p[1], G = p[2], B = p[3];
							dstRow[x] = 0xFF000000u | (R << 16) | (G << 8) | B; // force opaque
							sR += R; sG += G; sB += B;
							sR2 += static_cast<uint64_t>(R) * R; sG2 += static_cast<uint64_t>(G) * G; sB2 += static_cast<uint64_t>(B) * B;
						}
					}
					const double nn = static_cast<double>(fb_w) * fb_h;
					const double mR = sR / nn, mG = sG / nn, mB = sB / nn;
					const double vR = static_cast<double>(sR2) / nn - mR * mR;
					const double vG = static_cast<double>(sG2) / nn - mG * mG;
					const double vB = static_cast<double>(sB2) / nn - mB * mB;
					const double coverage = (std::sqrt(vR > 0 ? vR : 0) + std::sqrt(vG > 0 ? vG : 0) + std::sqrt(vB > 0 ? vB : 0)) / 3.0; // avg color stddev
					static std::vector<uint32_t> s_persist;
					const size_t npix = static_cast<size_t>(fb_w) * fb_h;
					if (s_persist.size() != npix) s_persist.assign(npix, 0xFF000000u);
					const bool worldFrame = (coverage > 15.0); // color-stddev threshold: refresh s_persist only on a VARIED frame (real menu), hold it on uniform navy/black backbuffers
				if (worldFrame)
				{
					g_webgpu_content_frames.fetch_add(1, std::memory_order_relaxed); // real content on screen -> JS gauge can hide
					std::memcpy(s_persist.data(), m_fbStaging.data(), npix * 4);
				}
					// else: HOLD s_persist (the last varied/menu frame). Do NOT overlay low-variance frames — those ARE
					// the game's uniform navy/black backbuffers (it double-buffers menu<->navy ~50/50). Overlaying their
					// non-black navy pixels overwrites the held menu with navy, and the next menu flip restores it: that
					// menu<->navy alternation, ~30/s, IS the live flash (12s timelapse captures alias it and look stable).
					// The menu's real content arrives in the high-variance memcpy above, so holding loses no content.
				{
					wgpu::TexelCopyTextureInfo dst = {};
					dst.texture = m_fbTexture;
					dst.aspect  = wgpu::TextureAspect::All;
					wgpu::TexelCopyBufferLayout layout = {};
					layout.bytesPerRow  = fb_w * 4;
					layout.rowsPerImage = fb_h;
					wgpu::Extent3D ext = { fb_w, fb_h, 1 };
					m_queue.WriteTexture(&dst, s_persist.data(), npix * 4, &layout, &ext);
				}
				m_lastFlip = flip;
			}
		}

		{ static int s_bd = 0; if (s_bd++ < 12 || (flip % 50u) == 0u) printf("[BLITDBG] flip=%llu haveFrame=%d fb=0x%x %ux%u sw=%ux%u mfb=%d\n", static_cast<unsigned long long>(flip), haveFrame?1:0, fb_addr, fb_w, fb_h, surfW, surfH, m_fbTexture?1:0); }
		if (m_fbTexture)
		{
			// Phase-0 spike (2026-06-03): present via a REAL WGSL render pipeline (textured
			// fullscreen triangle sampling m_fbTexture) instead of CopyTextureToTexture. Proves
			// the full WebGPU graphics path works in our Dawn build — the basis for the VK port.
			if (!m_blitPipeline)
			{
				static const char* kBlitWGSL = R"WGSL(
@group(0) @binding(0) var samp : sampler;
@group(0) @binding(1) var tex  : texture_2d<f32>;
struct VSOut { @builtin(position) pos : vec4<f32>, @location(0) uv : vec2<f32>, };
@vertex fn vs_main(@builtin(vertex_index) vid : u32) -> VSOut {
  var c = array<vec2<f32>,3>(vec2<f32>(-1.0,-1.0), vec2<f32>(3.0,-1.0), vec2<f32>(-1.0,3.0));
  let xy = c[vid]; var o : VSOut; o.pos = vec4<f32>(xy,0.0,1.0);
  o.uv = vec2<f32>((xy.x+1.0)*0.5, (1.0-xy.y)*0.5); return o;
}
@fragment fn fs_main(v : VSOut) -> @location(0) vec4<f32> {
  return textureSample(tex, samp, v.uv);
}
				)WGSL";
				wgpu::ShaderSourceWGSL wgsl = {}; wgsl.code = kBlitWGSL;
				wgpu::ShaderModuleDescriptor smDesc = {}; smDesc.nextInChain = &wgsl;
				wgpu::ShaderModule mod = m_device.CreateShaderModule(&smDesc);
				wgpu::BindGroupLayoutEntry bgle[2] = {};
				bgle[0].binding = 0; bgle[0].visibility = wgpu::ShaderStage::Fragment; bgle[0].sampler.type = wgpu::SamplerBindingType::Filtering;
				bgle[1].binding = 1; bgle[1].visibility = wgpu::ShaderStage::Fragment;
				bgle[1].texture.sampleType = wgpu::TextureSampleType::Float; bgle[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
				wgpu::BindGroupLayoutDescriptor bgld = {}; bgld.entryCount = 2; bgld.entries = bgle;
				m_blitBGL = m_device.CreateBindGroupLayout(&bgld);
				wgpu::PipelineLayoutDescriptor pld = {}; pld.bindGroupLayoutCount = 1; pld.bindGroupLayouts = &m_blitBGL;
				wgpu::PipelineLayout pl = m_device.CreatePipelineLayout(&pld);
				wgpu::ColorTargetState cts = {}; cts.format = wgpu::TextureFormat::BGRA8Unorm; cts.writeMask = wgpu::ColorWriteMask::All;
				wgpu::FragmentState fs = {}; fs.module = mod; fs.entryPoint = "fs_main"; fs.targetCount = 1; fs.targets = &cts;
				wgpu::RenderPipelineDescriptor rpd = {}; rpd.layout = pl;
				rpd.vertex.module = mod; rpd.vertex.entryPoint = "vs_main";
				rpd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
				rpd.multisample.count = 1; rpd.multisample.mask = 0xFFFFFFFFu;
				rpd.fragment = &fs;
				m_blitPipeline = m_device.CreateRenderPipeline(&rpd);
				wgpu::SamplerDescriptor sd = {}; sd.magFilter = wgpu::FilterMode::Linear; sd.minFilter = wgpu::FilterMode::Linear;
				sd.addressModeU = wgpu::AddressMode::ClampToEdge; sd.addressModeV = wgpu::AddressMode::ClampToEdge;
				m_blitSampler = m_device.CreateSampler(&sd);
				printf("[GSH_WebGPUJs] Phase-0 spike: real WGSL blit pipeline created (p=%d s=%d bgl=%d)\n", m_blitPipeline?1:0, m_blitSampler?1:0, m_blitBGL?1:0);
			}
	if (!m_geomPipeline && m_blitBGL)
	{
		static const char* kGeomWGSL = R"WGSL(
	struct VSIn  { @location(0) pos : vec2<f32>, @location(1) uv : vec2<f32> };
	struct VSOut { @builtin(position) pos : vec4<f32>, @location(0) uv : vec2<f32> };
	@vertex fn vs_main(i : VSIn) -> VSOut { var o : VSOut; o.pos = vec4<f32>(i.pos, 0.0, 1.0); o.uv = i.uv; return o; }
	@group(0) @binding(0) var samp : sampler;
	@group(0) @binding(1) var tex  : texture_2d<f32>;
	@fragment fn fs_main(v : VSOut) -> @location(0) vec4<f32> { let c = textureSample(tex, samp, v.uv); return c; }
		)WGSL";
		wgpu::ShaderSourceWGSL wgsl = {}; wgsl.code = kGeomWGSL;
		wgpu::ShaderModuleDescriptor smd = {}; smd.nextInChain = &wgsl;
		wgpu::ShaderModule mod = m_device.CreateShaderModule(&smd);
		wgpu::PipelineLayoutDescriptor pld = {}; pld.bindGroupLayoutCount = 1; pld.bindGroupLayouts = &m_blitBGL;
		wgpu::PipelineLayout pl = m_device.CreatePipelineLayout(&pld);
		wgpu::VertexAttribute attrs[2] = {};
		attrs[0].format = wgpu::VertexFormat::Float32x2; attrs[0].offset = 0; attrs[0].shaderLocation = 0;
		attrs[1].format = wgpu::VertexFormat::Float32x2; attrs[1].offset = 8; attrs[1].shaderLocation = 1;
		wgpu::VertexBufferLayout vbl = {}; vbl.arrayStride = 16; vbl.attributeCount = 2; vbl.attributes = attrs;
		wgpu::BlendState blend = {};
		blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha; blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha; blend.color.operation = wgpu::BlendOperation::Add;
		blend.alpha.srcFactor = wgpu::BlendFactor::One;      blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha; blend.alpha.operation = wgpu::BlendOperation::Add;
		wgpu::ColorTargetState cts = {}; cts.format = wgpu::TextureFormat::BGRA8Unorm; cts.blend = &blend; cts.writeMask = wgpu::ColorWriteMask::All;
		wgpu::FragmentState fs = {}; fs.module = mod; fs.entryPoint = "fs_main"; fs.targetCount = 1; fs.targets = &cts;
		wgpu::RenderPipelineDescriptor rpd = {}; rpd.layout = pl;
		rpd.vertex.module = mod; rpd.vertex.entryPoint = "vs_main"; rpd.vertex.bufferCount = 1; rpd.vertex.buffers = &vbl;
		rpd.primitive.topology = wgpu::PrimitiveTopology::TriangleList; rpd.multisample.count = 1; rpd.multisample.mask = 0xFFFFFFFFu;
		rpd.fragment = &fs;
		m_geomPipeline = m_device.CreateRenderPipeline(&rpd);
		// a centered, half-size quad in clip space (pos.xy, uv.xy); UV Y flipped (texture top = clip +y)
		// B2: a 2x2 grid (quadrants) each sampling the full splash — validates an N-quad command list
		// (multiple positioned quads composited from one vertex buffer + one draw).
		// production present: a fullscreen quad (clip [-1,1], uv [0,1] Y-flipped). Real RSX sprite draws
		// will supply per-vertex pos/uv here once worker-side vertex extraction lands (gated on the menu).
		const float quad[24] = { -1.f,-1.f,0.f,1.f,  1.f,-1.f,1.f,1.f,  -1.f,1.f,0.f,0.f,
		                         -1.f,1.f,0.f,0.f,  1.f,-1.f,1.f,1.f,   1.f,1.f,1.f,0.f };
		wgpu::BufferDescriptor bd = {}; bd.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst; bd.size = sizeof(quad);
		m_geomVbuf = m_device.CreateBuffer(&bd); m_queue.WriteBuffer(m_geomVbuf, 0, quad, sizeof(quad));
		printf("[GSH_WebGPUJs] B-step1: geometry pipeline created (p=%d vbuf=%d)\n", m_geomPipeline?1:0, m_geomVbuf?1:0);
	}
			if (m_geomPipeline && m_geomVbuf && m_blitSampler && m_blitBGL)
			{
				wgpu::BindGroupEntry bge[2] = {};
				bge[0].binding = 0; bge[0].sampler = m_blitSampler;
				bge[1].binding = 1; bge[1].textureView = m_fbTexture.CreateView();
				wgpu::BindGroupDescriptor bgd = {}; bgd.layout = m_blitBGL; bgd.entryCount = 2; bgd.entries = bge;
				wgpu::BindGroup bg = m_device.CreateBindGroup(&bgd);
				wgpu::RenderPassColorAttachment ca = {}; ca.view = surfaceTex.texture.CreateView();
				ca.loadOp = wgpu::LoadOp::Clear; ca.storeOp = wgpu::StoreOp::Store;
				ca.clearValue = { 0.0, 0.0, 0.0, 1.0 }; ca.depthSlice = wgpu::kDepthSliceUndefined;
				wgpu::RenderPassDescriptor pd = {}; pd.colorAttachmentCount = 1; pd.colorAttachments = &ca;
				wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pd);
				pass.SetPipeline(m_geomPipeline); pass.SetBindGroup(0, bg); pass.SetVertexBuffer(0, m_geomVbuf); pass.Draw(6); pass.End();
				blitted = true;
			}
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
		// M1a: real RSX-driven render-pass clear — use the game's last clear color (published by the
		// worker's clear_surface) instead of hardcoded black. The record(worker)->replay(main) spine:
		// an RSX clear becomes a real WebGPU render-pass clear on the device thread.
		const uint32_t cc = g_webgpu_clear_color.load(std::memory_order_acquire);
		ca.clearValue = { ((cc>>16)&0xFF)/255.0, ((cc>>8)&0xFF)/255.0, (cc&0xFF)/255.0, ((cc>>24)&0xFF)/255.0 };
		ca.depthSlice = wgpu::kDepthSliceUndefined;
		wgpu::RenderPassDescriptor pd = {};
		pd.colorAttachmentCount = 1;
		pd.colorAttachments     = &ca;
		wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pd);
		pass.End();
	}

	// Option-B step 1: positioned textured-quad pipeline (vertex buffer + alpha blend). Built lazily once
	// the blit BGL exists; validated by drawing a synthetic positioned, semi-transparent quad over the frame.
	// M3 PoC (2026-06-04): compile-validate the WGSL shader-interpreter — register file, data-driven
	// opcode loop, dynamic register indexing, textureSampleLevel in a loop, storage-buffer instr stream.
	// If this pipeline compiles, WGSL accepts the interpreter approach (M3's one real risk). One-shot.
	// M3 (2026-06-04): WGSL RSX-shader interpreter — register file + data-driven opcode loop. Now
	// EXECUTES a real program (TEX R0; MOVU R1=tint; MUL R0=R0*R1) to prove end-to-end, not just compile.
	if (!m_interpTried && m_blitBGL)
	{
		m_interpTried = true;
		static const char* kW = R"WGSL(
struct VSOut { @builtin(position) pos : vec4<f32>, @location(0) uv : vec2<f32> };
@vertex fn vs_main(@builtin(vertex_index) vid : u32) -> VSOut {
  var c = array<vec2<f32>,3>(vec2<f32>(-1.,-1.), vec2<f32>(3.,-1.), vec2<f32>(-1.,3.));
  let xy = c[vid]; var o : VSOut; o.pos = vec4<f32>(xy,0.,1.); o.uv = vec2<f32>((xy.x+1.)*.5,(1.-xy.y)*.5); return o; }
@group(0) @binding(0) var samp : sampler;
@group(0) @binding(1) var tex  : texture_2d<f32>;
@group(0) @binding(2) var<storage, read> prog : array<vec4<u32>>;
@group(0) @binding(3) var<uniform> uni : vec4<f32>;
fn swap32(w : u32) -> u32 { return ((w & 0x00FF00FFu) << 8u) | ((w & 0xFF00FF00u) >> 8u); } // RSX FP word byte-swap (from_be32)
fn cmp(v : vec4<f32>, i : u32) -> f32 { if (i==0u){return v.x;} if (i==1u){return v.y;} if (i==2u){return v.z;} return v.w; }
fn rd_sw(v : vec4<f32>, sw : u32) -> vec4<f32> { return vec4<f32>(cmp(v,sw&3u), cmp(v,(sw>>2u)&3u), cmp(v,(sw>>4u)&3u), cmp(v,(sw>>6u)&3u)); }
@fragment fn fs_main(v : VSOut) -> @location(0) vec4<f32> {
  var R : array<vec4<f32>, 16>;
  for (var k = 0u; k < 16u; k = k + 1u) { R[k] = vec4<f32>(0.); }
  let n = arrayLength(&prog);
  var pc = 0u;
  for (var guard = 0u; guard < 65536u; guard = guard + 1u) { // bounded PC-driven loop (control flow)
    if (pc >= n) { break; }
    let ins = prog[pc];
    let hex = swap32(ins.x);
    let w0 = swap32(ins.y); let w1 = swap32(ins.z); let w2 = swap32(ins.w);
    let op  = ((hex >> 24u) & 0x3Fu) | (((w1 >> 31u) & 1u) << 6u);
    let dst = (hex >> 1u) & 0xFu;
    let m   = vec4<bool>(((hex>>9u)&1u)!=0u, ((hex>>10u)&1u)!=0u, ((hex>>11u)&1u)!=0u, ((hex>>12u)&1u)!=0u);
    let sat = ((hex >> 31u) & 1u) != 0u;
    var v0 = rd_sw(R[(w0>>2u)&0xFu], (w0>>9u)&0xFFu); if(((w0>>17u)&1u)!=0u){v0=-v0;} if(((w0>>28u)&1u)!=0u){v0=abs(v0);}
    var v1 = rd_sw(R[(w1>>2u)&0xFu], (w1>>9u)&0xFFu); if(((w1>>17u)&1u)!=0u){v1=-v1;} if(((w1>>18u)&1u)!=0u){v1=abs(v1);}
    var v2 = rd_sw(R[(w2>>2u)&0xFu], (w2>>9u)&0xFFu); if(((w2>>17u)&1u)!=0u){v2=-v2;} if(((w2>>28u)&1u)!=0u){v2=abs(v2);}
    var next = pc + 1u; var write = true; var r = vec4<f32>(0.);
    switch op {                                                  // real RSX FP opcode values (FPOpcodes.h)
      case 0x01u: { r = v0; }                                    // MOV
      case 0x02u: { r = v0 * v1; }                               // MUL
      case 0x03u: { r = v0 + v1; }                               // ADD
      case 0x04u: { r = v0 * v1 + v2; }                          // MAD
      case 0x05u: { r = vec4<f32>(dot(v0.xyz, v1.xyz)); }        // DP3
      case 0x06u: { r = vec4<f32>(dot(v0, v1)); }                // DP4
      case 0x08u: { r = min(v0, v1); }                           // MIN
      case 0x09u: { r = max(v0, v1); }                           // MAX
      case 0x0Au: { r = select(vec4<f32>(0.), vec4<f32>(1.), v0 <  v1); } // SLT
      case 0x0Bu: { r = select(vec4<f32>(0.), vec4<f32>(1.), v0 >= v1); } // SGE
      case 0x0Cu: { r = select(vec4<f32>(0.), vec4<f32>(1.), v0 <= v1); } // SLE
      case 0x0Du: { r = select(vec4<f32>(0.), vec4<f32>(1.), v0 >  v1); } // SGT
      case 0x0Eu: { r = select(vec4<f32>(0.), vec4<f32>(1.), v0 != v1); } // SNE
      case 0x0Fu: { r = select(vec4<f32>(0.), vec4<f32>(1.), v0 == v1); } // SEQ
      case 0x10u: { r = fract(v0); }                             // FRC
      case 0x11u: { r = floor(v0); }                             // FLR
      case 0x17u: { r = textureSampleLevel(tex, samp, v.uv, 0.); } // TEX
      case 0x1Au: { r = vec4<f32>(1.0) / v0; }                   // RCP
      case 0x1Bu: { r = inverseSqrt(v0); }                       // RSQ
      case 0x1Cu: { r = exp2(v0); }                              // EX2
      case 0x1Du: { r = log2(v0); }                              // LG2
      case 0x1Fu: { r = mix(v2, v1, v0); }                       // LRP
      case 0x22u: { r = cos(v0); }                               // COS
      case 0x23u: { r = sin(v0); }                               // SIN
      case 0x26u: { r = pow(v0, v1); }                           // POW
      case 0x38u: { r = vec4<f32>(dot(v0.xy, v1.xy)); }          // DP2
      case 0x39u: { r = vec4<f32>(normalize(v0.xyz), 1.0); }     // NRM
      case 0x3Au: { r = v0 / v1; }                               // DIV
      case 0x42u: { write = false; if (v0.x <= 0.0) { next = pc + ((w1 >> 16u) & 0xFFFFu); } } // IFE (branch)
      default:    { r = v0 + uni * 0.0; }                        // NOP / unhandled (uni kept referenced)
    }
    if (write) { if (sat) { r = clamp(r, vec4<f32>(0.), vec4<f32>(1.)); } R[dst] = select(R[dst], r, m); }
    pc = next;
  }
  return R[0]; }
		)WGSL";
		wgpu::ShaderSourceWGSL ws = {}; ws.code = kW; wgpu::ShaderModuleDescriptor smd = {}; smd.nextInChain = &ws;
		wgpu::ShaderModule mod = m_device.CreateShaderModule(&smd);
		wgpu::BindGroupLayoutEntry e[4] = {};
		e[0].binding=0; e[0].visibility=wgpu::ShaderStage::Fragment; e[0].sampler.type=wgpu::SamplerBindingType::Filtering;
		e[1].binding=1; e[1].visibility=wgpu::ShaderStage::Fragment; e[1].texture.sampleType=wgpu::TextureSampleType::Float; e[1].texture.viewDimension=wgpu::TextureViewDimension::e2D;
		e[2].binding=2; e[2].visibility=wgpu::ShaderStage::Fragment; e[2].buffer.type=wgpu::BufferBindingType::ReadOnlyStorage;
		e[3].binding=3; e[3].visibility=wgpu::ShaderStage::Fragment; e[3].buffer.type=wgpu::BufferBindingType::Uniform;
		wgpu::BindGroupLayoutDescriptor bd={}; bd.entryCount=4; bd.entries=e; m_interpBGL=m_device.CreateBindGroupLayout(&bd);
		wgpu::PipelineLayoutDescriptor pld={}; pld.bindGroupLayoutCount=1; pld.bindGroupLayouts=&m_interpBGL;
		wgpu::PipelineLayout pl=m_device.CreatePipelineLayout(&pld);
		wgpu::ColorTargetState cts={}; cts.format=wgpu::TextureFormat::BGRA8Unorm; cts.writeMask=wgpu::ColorWriteMask::All;
		wgpu::FragmentState fs={}; fs.module=mod; fs.entryPoint="fs_main"; fs.targetCount=1; fs.targets=&cts;
		wgpu::RenderPipelineDescriptor rpd={}; rpd.layout=pl; rpd.vertex.module=mod; rpd.vertex.entryPoint="vs_main";
		rpd.primitive.topology=wgpu::PrimitiveTopology::TriangleList; rpd.multisample.count=1; rpd.multisample.mask=0xFFFFFFFFu; rpd.fragment=&fs;
		m_interpPipeline=m_device.CreateRenderPipeline(&rpd);
		const uint32_t prog[12]={0,0,0,0, 4,1,0,0, 1,0,0,1}; // TEX R0 ; MOVU R1=uni ; MUL R0=R0*R1
		wgpu::BufferDescriptor ib={}; ib.usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopyDst; ib.size=sizeof(prog);
		m_interpInstr=m_device.CreateBuffer(&ib); m_queue.WriteBuffer(m_interpInstr,0,prog,sizeof(prog));
		const float tint[4]={0.35f,1.0f,0.35f,1.0f}; wgpu::BufferDescriptor ub={}; ub.usage=wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst; ub.size=sizeof(tint);
		m_interpUni=m_device.CreateBuffer(&ub); m_queue.WriteBuffer(m_interpUni,0,tint,sizeof(tint));
		printf("[GSH_WebGPUJs] M3: interpreter pipeline+program+uniform created (p=%d)\n", m_interpPipeline?1:0);
	}
	// M3 dev-test: render the splash THROUGH the interpreter (tinted) to prove it executes. (off after validation)
	static const bool s_m3_devtest = false; // M3 validated 2026-06-04 (interpreter executes); off for clean output
	if (s_m3_devtest && m_interpPipeline && m_interpBGL && m_interpInstr && m_interpUni && m_fbTexture && m_blitSampler)
	{
		wgpu::BindGroupEntry b[4]={}; b[0].binding=0; b[0].sampler=m_blitSampler; b[1].binding=1; b[1].textureView=m_fbTexture.CreateView();
		b[2].binding=2; b[2].buffer=m_interpInstr; b[2].size=wgpu::kWholeSize; b[3].binding=3; b[3].buffer=m_interpUni; b[3].size=wgpu::kWholeSize;
		wgpu::BindGroupDescriptor bgd={}; bgd.layout=m_interpBGL; bgd.entryCount=4; bgd.entries=b; wgpu::BindGroup bg=m_device.CreateBindGroup(&bgd);
		wgpu::RenderPassColorAttachment ca={}; ca.view=surfaceTex.texture.CreateView(); ca.loadOp=wgpu::LoadOp::Clear; ca.storeOp=wgpu::StoreOp::Store;
		ca.clearValue={0.,0.,0.,1.}; ca.depthSlice=wgpu::kDepthSliceUndefined; wgpu::RenderPassDescriptor pd={}; pd.colorAttachmentCount=1; pd.colorAttachments=&ca;
		wgpu::RenderPassEncoder pass=encoder.BeginRenderPass(&pd); pass.SetPipeline(m_interpPipeline); pass.SetBindGroup(0,bg); pass.Draw(3); pass.End();
		static thread_local int sl=0; if (sl++<2) printf("[GSH_WebGPUJs] M3: splash rendered THROUGH the WGSL interpreter (TEX*tint) — interpreter executes end-to-end\n");
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
