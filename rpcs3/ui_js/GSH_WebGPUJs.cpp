#include "GSH_WebGPUJs.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

// Minimal WebGPU render path — see GSH_WebGPUJs.h for the integration
// plan. The real RSX-equivalent backend lives under Emu/RSX/WebGPU/ once
// it's written (plan-poc-ps3-webgpu.md §2.2). Until then `Render()` does
// the smallest thing that proves the wgpu::Surface → HTMLCanvas pipeline
// works end-to-end: acquire the next surface texture, encode a single
// render pass that either samples a reference texture (when uploaded via
// UploadReferenceTexture, e.g. for the side-by-side compare view) or
// clears to an animated color, then submits + presents.
//
// Why an animated clear (no-reference fallback):
//   - A static clear can be confused with a hung tab or a cached frame.
//   - The animation runs off m_framesRendered, so a screenshot at any
//     point shows a different color — proves the render loop is alive.
//   - When the rest of the pipeline lands (RSX → WGSL translator), this
//     becomes the "no-game-booted" splash; the if(m_booted) branch will
//     swap in real RSX command playback.
//
// Why the textured-quad path:
//   - Implements plan-poc-ps3-webgpu.md §3 Phase 4 sub-milestones P4.1
//     (solid-color quad — same pipeline minus the texture sample) and
//     P4.2 (textured quad — sampler binding works end-to-end).
//   - Lets the compare view (?compare=terraria) show the same image as
//     the reference panel even before RSX→WGSL translation is wired up,
//     so visual regressions in the WebGPU surface stack (clear, present,
//     pipeline cache rebuild on device-loss) are immediately obvious.
//   - The shader / sampler / bind-group plumbing here is the prototype
//     for what Emu/RSX/WebGPU/ will need; keep it small so it stays a
//     readable reference.

#ifdef HAS_WEBGPU

// Fullscreen-triangle WGSL — covers the viewport with a single oversized
// triangle that gets clipped to the [-1,1] x [-1,1] NDC box. Standard
// trick (Sascha Willems "fullscreen triangle"). Cheaper than a 2-tri
// quad because it avoids the diagonal seam between the two triangles.
//
// Texture-coord mapping:
//   - WebGPU NDC: y-up (+1 top, -1 bottom)
//   - WebGPU sampler UV: y-down (0 top, 1 bottom)
// → For vertex idx in [0,1,2]:
//      idx=0  → pos=(-1, +1)  uv=(0,0)  (top-left)
//      idx=1  → pos=(+3, +1)  uv=(2,0)  (extends past right edge)
//      idx=2  → pos=(-1, -3)  uv=(0,2)  (extends past bottom edge)
//   The triangle covers the visible viewport with interpolated UVs that
//   sweep (0,0) → (1,1) across the visible area.
static const char* kQuadShaderWGSL = R"WGSL(
struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) idx: u32) -> VsOut {
    let x = f32((idx << 1u) & 2u);
    let y = f32(idx & 2u);
    var out: VsOut;
    out.pos = vec4f(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
    out.uv  = vec2f(x, y);
    return out;
}

@group(0) @binding(0) var samp: sampler;
@group(0) @binding(1) var tex:  texture_2d<f32>;

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    return textureSample(tex, samp, in.uv);
}
)WGSL";

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

#ifdef HAS_WEBGPU
void CGSH_WebGPUJs::EnsureQuadPipeline()
{
	if (m_quadPipeline) return;
	if (!m_device) return;

	// 1. Shader module — vertex + fragment both compile from one source.
	wgpu::ShaderSourceWGSL wgslDesc = {};
	wgslDesc.code = kQuadShaderWGSL;
	wgpu::ShaderModuleDescriptor shaderDesc = {};
	shaderDesc.nextInChain = &wgslDesc;
	shaderDesc.label = "fullscreen-quad-shader";
	wgpu::ShaderModule shader = m_device.CreateShaderModule(&shaderDesc);
	if (!shader)
	{
		printf("[GSH_WebGPUJs] ERROR: shader module creation failed\n");
		return;
	}

	// 2. Bind-group layout — (sampler @binding 0, texture @binding 1).
	wgpu::BindGroupLayoutEntry bglEntries[2] = {};
	bglEntries[0].binding = 0;
	bglEntries[0].visibility = wgpu::ShaderStage::Fragment;
	bglEntries[0].sampler.type = wgpu::SamplerBindingType::Filtering;
	bglEntries[1].binding = 1;
	bglEntries[1].visibility = wgpu::ShaderStage::Fragment;
	bglEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
	bglEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
	bglEntries[1].texture.multisampled = false;

	wgpu::BindGroupLayoutDescriptor bglDesc = {};
	bglDesc.entryCount = 2;
	bglDesc.entries = bglEntries;
	m_quadBindGroupLayout = m_device.CreateBindGroupLayout(&bglDesc);

	wgpu::PipelineLayoutDescriptor plDesc = {};
	plDesc.bindGroupLayoutCount = 1;
	plDesc.bindGroupLayouts = &m_quadBindGroupLayout;
	wgpu::PipelineLayout pipelineLayout = m_device.CreatePipelineLayout(&plDesc);

	// 3. Sampler — bilinear, clamp. The reference image fills the canvas
	//    even when canvas aspect ≠ image aspect; clamping avoids edge
	//    smearing at the extrapolated UVs.
	wgpu::SamplerDescriptor samplerDesc = {};
	samplerDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
	samplerDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
	samplerDesc.addressModeW = wgpu::AddressMode::ClampToEdge;
	samplerDesc.magFilter = wgpu::FilterMode::Linear;
	samplerDesc.minFilter = wgpu::FilterMode::Linear;
	samplerDesc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
	samplerDesc.maxAnisotropy = 1;
	m_quadSampler = m_device.CreateSampler(&samplerDesc);

	// 4. Render pipeline — no vertex buffers (geometry built in shader),
	//    triangle-list topology, no depth/stencil, single color target
	//    matching the configured surface format.
	wgpu::ColorTargetState colorTarget = {};
	colorTarget.format = wgpu::TextureFormat::BGRA8Unorm;
	colorTarget.writeMask = wgpu::ColorWriteMask::All;

	wgpu::FragmentState fragState = {};
	fragState.module = shader;
	fragState.entryPoint = "fs_main";
	fragState.targetCount = 1;
	fragState.targets = &colorTarget;

	wgpu::RenderPipelineDescriptor pipeDesc = {};
	pipeDesc.label = "fullscreen-quad-pipeline";
	pipeDesc.layout = pipelineLayout;
	pipeDesc.vertex.module = shader;
	pipeDesc.vertex.entryPoint = "vs_main";
	pipeDesc.vertex.bufferCount = 0;
	pipeDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
	pipeDesc.primitive.cullMode = wgpu::CullMode::None;
	pipeDesc.multisample.count = 1;
	pipeDesc.multisample.mask = 0xFFFFFFFF;
	pipeDesc.fragment = &fragState;

	m_quadPipeline = m_device.CreateRenderPipeline(&pipeDesc);
	if (!m_quadPipeline)
	{
		printf("[GSH_WebGPUJs] ERROR: pipeline creation failed\n");
		return;
	}
	printf("[GSH_WebGPUJs] EnsureQuadPipeline OK\n");
}

void CGSH_WebGPUJs::UploadReferenceTexture(const uint8_t* rgba, int width, int height)
{
	if (!m_device || !m_queue) { printf("[GSH_WebGPUJs] UploadReferenceTexture: no device/queue\n"); return; }
	if (!rgba || width <= 0 || height <= 0) { printf("[GSH_WebGPUJs] UploadReferenceTexture: bad args w=%d h=%d ptr=%p\n", width, height, rgba); return; }

	EnsureQuadPipeline();
	if (!m_quadBindGroupLayout) { printf("[GSH_WebGPUJs] UploadReferenceTexture: no BGL\n"); return; }

	// (Re)create texture if the dimensions changed; otherwise reuse to
	// avoid the per-upload allocator churn warned about in
	// plan-poc-ps3-webgpu.md §1.3. RGBA8Unorm is what the JS side packs.
	if (!m_refTexture || width != m_refTextureWidth || height != m_refTextureHeight)
	{
		wgpu::TextureDescriptor texDesc = {};
		texDesc.label = "reference-image-texture";
		texDesc.size.width = static_cast<uint32_t>(width);
		texDesc.size.height = static_cast<uint32_t>(height);
		texDesc.size.depthOrArrayLayers = 1;
		texDesc.mipLevelCount = 1;
		texDesc.sampleCount = 1;
		texDesc.dimension = wgpu::TextureDimension::e2D;
		texDesc.format = wgpu::TextureFormat::RGBA8Unorm;
		texDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
		m_refTexture = m_device.CreateTexture(&texDesc);
		if (!m_refTexture)
		{
			printf("[GSH_WebGPUJs] UploadReferenceTexture: CreateTexture failed (%dx%d)\n", width, height);
			return;
		}
		m_refTextureView = m_refTexture.CreateView();
		m_refTextureWidth = width;
		m_refTextureHeight = height;

		wgpu::BindGroupEntry bgEntries[2] = {};
		bgEntries[0].binding = 0;
		bgEntries[0].sampler = m_quadSampler;
		bgEntries[1].binding = 1;
		bgEntries[1].textureView = m_refTextureView;
		wgpu::BindGroupDescriptor bgDesc = {};
		bgDesc.layout = m_quadBindGroupLayout;
		bgDesc.entryCount = 2;
		bgDesc.entries = bgEntries;
		m_refBindGroup = m_device.CreateBindGroup(&bgDesc);
	}

	// writeTexture requires bytesPerRow be a multiple of 256
	// (kCopyBytesPerRowAlignment). If the source isn't padded already we
	// stage it locally — keeps the JS upload path simple at the cost of
	// one extra heap copy per upload. Reference image upload is one-shot,
	// not per-frame, so the cost is negligible.
	const uint32_t tightBpr  = static_cast<uint32_t>(width) * 4u;
	const uint32_t paddedBpr = (tightBpr + 255u) & ~255u;
	const uint8_t* uploadPtr = rgba;
	std::vector<uint8_t> padded;
	if (paddedBpr != tightBpr)
	{
		padded.assign(static_cast<size_t>(paddedBpr) * static_cast<size_t>(height), 0);
		for (int y = 0; y < height; ++y)
		{
			std::memcpy(padded.data() + static_cast<size_t>(y) * paddedBpr,
			            rgba + static_cast<size_t>(y) * tightBpr,
			            tightBpr);
		}
		uploadPtr = padded.data();
	}

	wgpu::TexelCopyTextureInfo dst = {};
	dst.texture = m_refTexture;
	dst.mipLevel = 0;
	dst.origin = {0, 0, 0};
	dst.aspect = wgpu::TextureAspect::All;

	wgpu::TexelCopyBufferLayout layout = {};
	layout.offset = 0;
	layout.bytesPerRow = paddedBpr;
	layout.rowsPerImage = static_cast<uint32_t>(height);

	wgpu::Extent3D extent = {};
	extent.width = static_cast<uint32_t>(width);
	extent.height = static_cast<uint32_t>(height);
	extent.depthOrArrayLayers = 1;

	const size_t byteSize = static_cast<size_t>(paddedBpr) * static_cast<size_t>(height);
	m_queue.WriteTexture(&dst, uploadPtr, byteSize, &layout, &extent);
	printf("[GSH_WebGPUJs] UploadReferenceTexture OK %dx%d (tightBpr=%u paddedBpr=%u)\n",
	       width, height, tightBpr, paddedBpr);
}

void CGSH_WebGPUJs::DrawReferenceQuad(wgpu::RenderPassEncoder& pass)
{
	if (!m_quadPipeline || !m_refBindGroup) return;
	pass.SetPipeline(m_quadPipeline);
	pass.SetBindGroup(0, m_refBindGroup);
	pass.Draw(3, 1, 0, 0);
}
#endif // HAS_WEBGPU

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

	// Pick the load op + clear color. When a reference texture is loaded,
	// the textured-quad covers the whole viewport so the clear value
	// doesn't matter — keep load op Clear with opaque black as a sane
	// default. Otherwise fall back to the animated splash color so the
	// user can see the render loop is alive.
	const bool haveRef = (m_refBindGroup != nullptr);
	const float t = static_cast<float>(m_framesRendered) * 0.01f;
	wgpu::Color clearColor;
	if (haveRef)
	{
		clearColor = {0.0, 0.0, 0.0, 1.0};
	}
	else if (m_booted)
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
	if (haveRef)
	{
		DrawReferenceQuad(pass);
	}
	// else: clear-only pass — Phase 4 fills this with RSX-derived geometry.
	pass.End();

	wgpu::CommandBuffer cmd = encoder.Finish();
	m_queue.Submit(1, &cmd);

	// Note: in emdawnwebgpu the swap-chain is auto-presented when the
	// JS event loop returns — no explicit Surface.Present() needed.

	m_framesRendered++;
	if ((m_framesRendered % 60) == 1)
	{
		printf("[GSH_WebGPUJs] Render frame=%d booted=%d ref=%d clear=(%.2f,%.2f,%.2f)\n",
		       m_framesRendered, m_booted ? 1 : 0, haveRef ? 1 : 0,
		       static_cast<float>(clearColor.r),
		       static_cast<float>(clearColor.g),
		       static_cast<float>(clearColor.b));
	}
	return true;
#else
	return false;
#endif
}
