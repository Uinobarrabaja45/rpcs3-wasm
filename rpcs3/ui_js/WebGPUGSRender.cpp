#include "stdafx.h"
#include "WebGPUGSRender.h"
#include "Emu/Memory/vm.h"
#include "Emu/RSX/gcm_enums.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/rsx_utils.h"   // get_vertex_offset_from_base (CUT-1 rasterizer vertex address resolution)

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
// M1a (record/replay): latest RSX clear color (A8R8G8B8), published by clear_surface on the RSX
// worker, consumed by GSH::Render on the main thread for a real render-pass clear.
std::atomic<u32> g_webgpu_clear_color{0xFF000000u};
// M2a (record/replay): bound texture of the frame's last textured draw, published at flip() so
// GSH samples the GAME's real texture via the GPU pipeline (not the software P2-DRAW VRAM copy).
std::atomic<u64> g_webgpu_tex_host{0};
std::atomic<u32> g_webgpu_tex_w{0};
std::atomic<u32> g_webgpu_tex_h{0};
std::atomic<u32> g_webgpu_tex_pitch{0};
std::atomic<u64> g_webgpu_tex_flip{0}; // flip count this texture is valid for (0 = none)
static u64 s_frame_tex_host = 0; static u32 s_frame_tex_w = 0, s_frame_tex_h = 0, s_frame_tex_pitch = 0;
static bool s_frame_has_tex = false; // worker-only: set in end(), published in flip()

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
#ifdef __EMSCRIPTEN__
	// Wall-B diagnostic (2026-06-02): is the game actually issuing scene draws?
	// If end() fires with real vertex counts, the scene (incl. the 505 splash) IS being
	// submitted to RSX and only this nop-draw backend is dropping it — i.e. a real
	// RSX→WebGPU translator would surface it, and the boot deadlock (Wall A) is NOT what
	// gates the first frame's pixels. If end() rarely/never fires, the game isn't drawing
	// yet (gated by the deadlock). Log first draws + periodic, with the surface the draw
	// targets (color offset A) so we know whether draws hit the displayed buffer.
	{
		static thread_local u64 s_draws = 0;
		const u64 n = ++s_draws;
		const auto& dc = rsx::method_registers.current_draw_clause;
		// crash-safe: get_elements_count() asserts when draw_command_ranges is empty and the
		// command isn't an inlined array — guard with empty().
		const u32 verts = dc.empty() ? 0u : dc.get_elements_count();
		// Log EVERY non-empty draw (real geometry — the scene/splash!) + a few empties + periodic.
		if (verts > 0 || n <= 8 || (n % 2000) == 0)
		{
			printf("[WGPU-DRAW] #%llu prim=%u verts=%u empty=%d surfA_off=0x%x surf=%ux%u\n",
				static_cast<unsigned long long>(n),
				static_cast<unsigned>(dc.primitive),
				static_cast<unsigned>(verts),
				dc.empty() ? 1 : 0,
				static_cast<unsigned>(rsx::method_registers.surface_offset(0)),
				static_cast<unsigned>(rsx::method_registers.surface_clip_width()),
				static_cast<unsigned>(rsx::method_registers.surface_clip_height()));
		}
	}
	// DRAWDBG (2026-06-04): INLDUMP never fired across 157K draws ⇒ command != inlined_array. The menu draws
	// are command=array/indexed and verts=0 is the dangling-barrier artifact: get_elements_count() returns the
	// CURRENT range (a trailing count-0 barrier), while the REAL geometry is the sum over ALL ranges. Log
	// UNCONDITIONALLY (first 8 + periodic): command, range totals, is_immediate (push-buffer vs DMA arrays),
	// and the per-attribute layout (type/size/offset/stride) so we can locate the real vertex data next.
	{
		const auto& dcd = rsx::method_registers.current_draw_clause;
		static thread_local u64 s_id = 0;
		const u64 m = ++s_id;
		if (m <= 8 || (m % 50000u) == 0u)
		{
			printf("[DRAWDBG] #%llu cmd=%u prim=%u empty=%d immediate=%d nranges=%u TOTAL=%u curcount=%u curfirst=%u\n",
				static_cast<unsigned long long>(m), static_cast<unsigned>(dcd.command),
				static_cast<unsigned>(dcd.primitive), dcd.empty() ? 1 : 0,
				dcd.is_immediate_draw ? 1 : 0,
				static_cast<unsigned>(dcd.debug_range_count()),
				static_cast<unsigned>(dcd.debug_total_elements()),
				dcd.empty() ? 0u : static_cast<unsigned>(dcd.get_elements_count()),
				dcd.empty() ? 0u : static_cast<unsigned>(dcd.get_range().first));
			char buf[640]; int bp = 0;
			bp += snprintf(buf + bp, sizeof(buf) - bp, "[ATTRDBG] #%llu arrays:", static_cast<unsigned long long>(m));
			for (u8 i = 0; i < rsx::limits::vertex_count; i++)
			{
				const auto& vi = rsx::method_registers.vertex_arrays_info[i];
				if (vi.size() > 0)
					bp += snprintf(buf + bp, sizeof(buf) - bp, " a%u(t=%u s=%u off=0x%x str=%u)",
						static_cast<unsigned>(i), static_cast<unsigned>(vi.type()),
						static_cast<unsigned>(vi.size()), static_cast<unsigned>(vi.offset()),
						static_cast<unsigned>(vi.stride()));
			}
			for (u8 i = 0; i < rsx::limits::vertex_count; i++)
			{
				const auto& ri = rsx::method_registers.register_vertex_info[i];
				if (ri.size > 0)
					bp += snprintf(buf + bp, sizeof(buf) - bp, " R%u(t=%u s=%u)",
						static_cast<unsigned>(i), static_cast<unsigned>(ri.type), static_cast<unsigned>(ri.size));
			}
			printf("%s\n", buf);
			if (m == 1)
			{
				for (int r = 0; r < 4; r++)
				{
					union { u32 u; float f; } a, b, c, d;
					a.u = rsx::method_registers.transform_constants[r][0];
					b.u = rsx::method_registers.transform_constants[r][1];
					c.u = rsx::method_registers.transform_constants[r][2];
					d.u = rsx::method_registers.transform_constants[r][3];
					printf("[VPCONST] c%d: %.4f %.4f %.4f %.4f\n", r, a.f, b.f, c.f, d.f);
				}
			}
		}
	}
	// P2 REAL-DRAW PATH (2026-06-03): when the game issues a real textured-quad draw (the splash IS one),
	// decode the bound texture (fragment_textures[0]) and rasterize it into the color surface, which flip()
	// presents. This is the GAME-DRIVEN splash renderer — paired with the synthetic dev-test in
	// clear_surface that validated the rasterize→VRAM→flip→canvas path. The game currently emits only
	// degenerate draws (P1 gates the real ones); this is READY for the moment P1 lands. Handles
	// A8R8G8B8 (the logo's format); scales the texture across the surface (full-screen quad).
	// CUT-1 RASTERIZER (2026-06-04): the menu draws are REAL textured QUADS (cmd=array, prim=quads, DMA vertex
	// arrays): a0=pos(2 float, SCREEN-space 0..960/0..540 per the ortho VPCONST), a3=color(4 unorm8), a8=uv(2
	// float), stride 20. Decode the real geometry and fill each quad's screen AABB into the A8R8G8B8 color
	// surface with its REAL vertex color, then point M2a at that surface so flip() blits it 1:1 (clean
	// 960x540 — no green-noise format mismatch). Validates address-resolution + vertex-decode + rasterize +
	// present. CUT-2 swaps the solid fill for per-pixel texture sampling (the real art).
	{
		const auto& dc = rsx::method_registers.current_draw_clause;
		if (!dc.empty() && dc.command == rsx::draw_command::array && dc.primitive == rsx::primitive_type::quads)
		{
			const u32 saddr  = rsx::get_address(rsx::method_registers.surface_offset(0), CELL_GCM_LOCATION_LOCAL);
			const u32 sw     = rsx::method_registers.surface_clip_width();
			const u32 sh     = rsx::method_registers.surface_clip_height();
			const u32 spitch = rsx::method_registers.surface_pitch(0);
			const auto& a0 = rsx::method_registers.vertex_arrays_info[0]; // position (2x float)
			const auto& a3 = rsx::method_registers.vertex_arrays_info[3]; // color (4x unorm8)
			const auto& a8 = rsx::method_registers.vertex_arrays_info[8]; // texcoord (2x float)
			const u32 stride = a0.stride();
			if (saddr && sw && sh && spitch && stride >= 20u && a0.size() >= 2u)
			{
				u8* const sbase = static_cast<u8*>(vm::base(saddr));
				// Per-frame clear: the menu's full-screen RTT composite normally re-fills surf=0x0 every frame,
				// but that's the opaque-black layer we skip — so without it surf=0x0 accumulates STALE pixels
				// (the green/pink garbage seen between frames). Clear to opaque black once per frame, on the
				// first draw after a flip (g_webgpu_flip_count changes at flip()).
				{
					static thread_local u64 s_lastClearFlip = ~0ull;
					const u64 cf = g_webgpu_flip_count.load(std::memory_order_relaxed);
					if (cf != s_lastClearFlip)
					{
						s_lastClearFlip = cf;
						for (u32 yy = 0; yy < sh; yy++)
						{
							be_t<u32>* crow = reinterpret_cast<be_t<u32>*>(sbase + static_cast<u64>(yy) * spitch);
							for (u32 xx = 0; xx < sw; xx++) crow[xx] = 0xff000000u;
						}
					}
				}
				const u32 vbase = rsx::method_registers.vertex_data_base_offset();
				const auto resolve = [&](u32 rawoff) -> const u8* {
					const u32 a = rsx::get_address(rsx::get_vertex_offset_from_base(vbase, rawoff & 0x7fffffffu), rawoff >> 31);
					return a ? static_cast<const u8*>(vm::base(a)) : nullptr;
				};
				const u8* const pbase = resolve(a0.offset());
				const u8* const cbase = (a3.size() >= 4u) ? resolve(a3.offset()) : nullptr;
				const u8* const ubase = (a8.size() >= 2u) ? resolve(a8.offset()) : nullptr;
				u32 nverts = dc.debug_total_elements();
				if (nverts > 65536u) nverts = 65536u; // runaway guard
				const auto rdf = [](const u8* p) -> float { return reinterpret_cast<const be_t<f32>*>(p)->value(); };
				// CUT-2: per-pixel sampling of the bound texture (fragment_textures[0]). Handle the LINEAR
				// (CELL_GCM_TEXTURE_LN, bit 0x20) formats the menu uses: A8R8G8B8(0x85), A1R5G5B5(0x82),
				// A4R4G4B4(0x83). Swizzled/unhandled → flat vertex color (graceful fallback). The 1x1-white
				// solid-fill quads are swizzled 0x85 → fall back to their vertex tint (the gray/fade fills).
				const auto& tex = rsx::method_registers.fragment_textures[0];
				const u32 tfmt = tex.format();
				const u32 tbf = tfmt & ~0x60u;             // strip LN(0x20)/UN(0x40)
				const bool tlin = (tfmt & 0x20u) != 0u;    // linear (row-major) vs swizzled
				const u32 tw = tex.width(), th = tex.height();
				const u32 tpitch = tex.pitch() ? tex.pitch() : tw * 4u;
				const u32 taddr = (tex.offset() && tw && th) ? rsx::get_address(tex.offset(), tex.location()) : 0u;
				const u8* const tbase = taddr ? static_cast<const u8*>(vm::base(taddr)) : nullptr;
				const bool isPow2 = tw && th && (tw & (tw - 1u)) == 0u && (th & (th - 1u)) == 0u;
				const bool texOK = tbase && tw && th && (
					(tlin && (tbf == 0x85u || tbf == 0x82u || tbf == 0x83u || tbf == 0x86u)) ||  // linear formats
					(!tlin && isPow2 && (tbf == 0x85u || tbf == 0x82u || tbf == 0x83u)));         // swizzled pow2 (Morton)
				const auto sampleTex = [&](float u, float v) -> u32 { // returns ARGB (real alpha)
					int tx = static_cast<int>(u * static_cast<float>(tw)); int ty = static_cast<int>(v * static_cast<float>(th));
					tx = tx < 0 ? 0 : (tx >= static_cast<int>(tw) ? static_cast<int>(tw) - 1 : tx);
					ty = ty < 0 ? 0 : (ty >= static_cast<int>(th) ? static_cast<int>(th) - 1 : ty);
					if (tbf == 0x86u) // DXT1 (CELL_GCM_TEXTURE_COMPRESSED_DXT1): 4x4 blocks, 8 bytes; tpitch = block-row stride
					{
						const u8* blk = tbase + static_cast<u64>(ty >> 2) * tpitch + static_cast<u64>(tx >> 2) * 8u;
						const u32 c0 = (u32(blk[0])<<8)|blk[1], c1 = (u32(blk[2])<<8)|blk[3];      // RGB565 (BE)
						const u32 bits = u32(blk[4])|(u32(blk[5])<<8)|(u32(blk[6])<<16)|(u32(blk[7])<<24);
						const u32 sel = (bits >> (2u*((static_cast<u32>(ty)&3u)*4u + (static_cast<u32>(tx)&3u)))) & 3u;
						const u32 r0=((c0>>11)&0x1fu)<<3,g0=((c0>>5)&0x3fu)<<2,b0=(c0&0x1fu)<<3;
						const u32 r1=((c1>>11)&0x1fu)<<3,g1=((c1>>5)&0x3fu)<<2,b1=(c1&0x1fu)<<3;
						u32 R,G,B,A=255u;
						if (sel==0u){R=r0;G=g0;B=b0;} else if (sel==1u){R=r1;G=g1;B=b1;}
						else if (c0>c1){ if(sel==2u){R=(2u*r0+r1)/3u;G=(2u*g0+g1)/3u;B=(2u*b0+b1)/3u;} else {R=(r0+2u*r1)/3u;G=(g0+2u*g1)/3u;B=(b0+2u*b1)/3u;} }
						else { if(sel==2u){R=(r0+r1)/2u;G=(g0+g1)/2u;B=(b0+b1)/2u;} else {R=0u;G=0u;B=0u;A=0u;} } // sel==3 → transparent
						return (A<<24)|(R<<16)|(G<<8)|B;
					}
					// Non-DXT texel byte pointer: linear row-major, or swizzled Morton (interleave x,y bits —
					// handles non-square pow2). 1x1 textures fall out naturally (tx=ty=0 → tbase).
					const u32 bpp = (tbf == 0x85u) ? 4u : 2u;
					const u8* p;
					if (tlin) { p = tbase + static_cast<u64>(ty) * tpitch + static_cast<u64>(tx) * bpp; }
					else
					{
						u32 sx = static_cast<u32>(tx), sy = static_cast<u32>(ty), off = 0u, shift = 0u, mx = tw - 1u, my = th - 1u;
						while (mx || my) { if (mx){ off |= (sx&1u)<<shift; sx>>=1; ++shift; mx>>=1; } if (my){ off |= (sy&1u)<<shift; sy>>=1; ++shift; my>>=1; } }
						p = tbase + static_cast<u64>(off) * bpp;
					}
					if (tbf == 0x85u) { return (u32(p[0])<<24)|(u32(p[1])<<16)|(u32(p[2])<<8)|u32(p[3]); }
					const u32 w16 = (u32(p[0])<<8) | p[1];
					if (tbf == 0x82u) { const u32 A=(w16&0x8000u)?255u:0u,R=((w16>>10)&0x1fu)<<3,G=((w16>>5)&0x1fu)<<3,B=(w16&0x1fu)<<3; return (A<<24)|(R<<16)|(G<<8)|B; }
					const u32 A=((w16>>12)&0xfu)*17u,R=((w16>>8)&0xfu)*17u,G=((w16>>4)&0xfu)*17u,B=(w16&0xfu)*17u; return (A<<24)|(R<<16)|(G<<8)|B; // 0x83
				};
				static thread_local u64 s_rq = 0;
				if (pbase)
				for (u32 q = 0; q + 4u <= nverts; q += 4u)
				{
					float px[4], py[4], pu[4], pv[4];
					for (int k = 0; k < 4; k++)
					{
						const u8* pp = pbase + static_cast<u64>(q + k) * stride;
						px[k] = rdf(pp); py[k] = rdf(pp + 4);
						if (ubase) { const u8* up = ubase + static_cast<u64>(q + k) * stride; pu[k] = rdf(up); pv[k] = rdf(up + 4); }
						else { pu[k] = pv[k] = 0.0f; }
					}
					float minx=px[0],maxx=px[0],miny=py[0],maxy=py[0],minu=pu[0],maxu=pu[0],minv=pv[0],maxv=pv[0];
					for (int k = 1; k < 4; k++)
					{
						minx=px[k]<minx?px[k]:minx; maxx=px[k]>maxx?px[k]:maxx; miny=py[k]<miny?py[k]:miny; maxy=py[k]>maxy?py[k]:maxy;
						minu=pu[k]<minu?pu[k]:minu; maxu=pu[k]>maxu?pu[k]:maxu; minv=pv[k]<minv?pv[k]:minv; maxv=pv[k]>maxv?pv[k]:maxv;
					}
					const int x0 = minx<0.0f?0:static_cast<int>(minx);
					const int y0 = miny<0.0f?0:static_cast<int>(miny);
					const int x1 = maxx>static_cast<float>(sw)?static_cast<int>(sw):static_cast<int>(maxx+0.5f);
					const int y1 = maxy>static_cast<float>(sh)?static_cast<int>(sh):static_cast<int>(maxy+0.5f);
					u32 vR=255,vG=255,vB=255,vA=255; // vertex color (a3, vertex 0) modulator
					if (cbase) { const u8* cp = cbase + static_cast<u64>(q)*stride; vR=cp[0];vG=cp[1];vB=cp[2];vA=cp[3]; }
					const float wdt=(maxx>minx)?(maxx-minx):1.0f, hgt=(maxy>miny)?(maxy-miny):1.0f;
					// Skip full-screen OPAQUE-BLACK layers (empty render-to-texture composites that would black
					// out the menu). Classify by sampling 5 points; skip only if ALL are opaque (~black).
					const bool fullscreen = (maxx-minx) > static_cast<float>(sw)*0.9f && (maxy-miny) > static_cast<float>(sh)*0.9f;
					bool blackRTT = false;
					if (fullscreen && texOK)
					{
						blackRTT = true;
						static const float sp[5][2] = {{0.5f,0.5f},{0.1f,0.1f},{0.9f,0.1f},{0.1f,0.9f},{0.9f,0.9f}};
						for (int sgi = 0; sgi < 5; sgi++)
						{
							const u32 tc = sampleTex(minu+(maxu-minu)*sp[sgi][0], minv+(maxv-minv)*sp[sgi][1]);
							if (((tc>>24)&0xffu) < 0xc0u || (tc & 0x00ffffffu) != 0u) { blackRTT = false; break; }
						}
					}
					// Skip a quad sampling the ENTIRE large atlas (uv≈0..1 of a >1024² texture) — an artifact that
					// dumps the raw atlas sheet over the menu (no real sprite samples a whole 3900² atlas).
					const bool fullAtlasDump = texOK && tw > 1024u && th > 1024u && minu < 0.02f && maxu > 0.98f && minv < 0.02f && maxv > 0.98f;
					if (!blackRTT && !fullAtlasDump)
					for (int y = y0; y < y1; y++)
					{
						be_t<u32>* row = reinterpret_cast<be_t<u32>*>(sbase + static_cast<u64>(y)*spitch);
						const float vv = minv + ((static_cast<float>(y)+0.5f - miny)/hgt)*(maxv-minv);
						for (int x = x0; x < x1; x++)
						{
							u32 fR,fG,fB,fA;
							if (texOK)
							{
								const float uu = minu + ((static_cast<float>(x)+0.5f - minx)/wdt)*(maxu-minu);
								const u32 t = sampleTex(uu, vv);
								fA=(((t>>24)&0xffu)*vA)/255u; fR=(((t>>16)&0xffu)*vR)/255u; fG=(((t>>8)&0xffu)*vG)/255u; fB=((t&0xffu)*vB)/255u;
							}
							else { fA=vA; fR=vR; fG=vG; fB=vB; } // flat vertex color (swizzled/unhandled tex, e.g. DXT1 text)
							if (fA == 0u) continue;                                                     // fully transparent → skip
							if (fA >= 250u) { row[x] = 0xff000000u | (fR<<16)|(fG<<8)|fB; continue; }   // opaque → overwrite
							const u32 d = row[x];                                                       // else src-over blend onto surface
							const u32 dR=(d>>16)&0xffu, dG=(d>>8)&0xffu, dB=d&0xffu, ia=255u-fA;
							row[x] = 0xff000000u | (((fR*fA+dR*ia)/255u)<<16) | (((fG*fA+dG*ia)/255u)<<8) | ((fB*fA+dB*ia)/255u);
						}
					}
					// Diagnostics with surface/texture offsets to map the multi-pass (RTT) flow.
					static thread_local u64 s_rq2 = 0;
					const bool small = (maxx - minx) < static_cast<float>(sw) * 0.8f;
					const u32 centerTexel = texOK ? sampleTex((minu + maxu) * 0.5f, (minv + maxv) * 0.5f) : 0u;
					const u32 surfoff = rsx::method_registers.surface_offset(0);
					const u32 texoff = tex.offset();
					if (s_rq < 6u)
					{
						s_rq++;
						printf("[RAST] q#%llu pos(%.0f,%.0f)-(%.0f,%.0f) surf=0x%x tex=0x%x tfmt=0x%x texOK=%d blackRTT=%d %ux%u ctex=%08x vcol=%02x%02x%02x%02x\n",
							static_cast<unsigned long long>(s_rq), minx, miny, maxx, maxy, surfoff, texoff, tfmt, texOK?1:0, blackRTT?1:0, tw, th, centerTexel, vR,vG,vB,vA);
					}
					if (small && s_rq2 < 12u)
					{
						s_rq2++;
						printf("[RASTSP] s#%llu pos(%.0f,%.0f)-(%.0f,%.0f) surf=0x%x tex=0x%x uv(%.2f,%.2f)-(%.2f,%.2f) tfmt=0x%x texOK=%d %ux%u ctex=%08x vcol=%02x%02x%02x%02x\n",
							static_cast<unsigned long long>(s_rq2), minx, miny, maxx, maxy, surfoff, texoff, minu,minv,maxu,maxv, tfmt, texOK?1:0, tw, th, centerTexel, vR,vG,vB,vA);
					}
				}
				// Present the rasterized surface via M2a: clean A8R8G8B8 sw×sh → flip blits it 1:1.
				s_frame_tex_host = reinterpret_cast<uintptr_t>(sbase);
				s_frame_tex_w = sw; s_frame_tex_h = sh; s_frame_tex_pitch = spitch; s_frame_has_tex = true;
			}
		}
	}
#endif
	// Same body as NullGSRender — no actual draw. Phase 7+ will replace
	// this with WGSL pipeline dispatch translated from RSX FP/VP.
	execute_nop_draw();
	rsx::thread::end();
}

// Renderer step 1: honor color clears by writing the clear color straight into the guest VRAM
// color surface, so the existing flip→blit presents it. The 3D draw path is still nop, but this
// surfaces the game's real per-frame background/clear color instead of a permanent black frame —
// and proves the RSX-state → VRAM → blit → canvas pipeline end-to-end.
void WebGPUGSRender::clear_surface(u32 arg)
{
#ifdef __EMSCRIPTEN__
	if ((arg & RSX_GCM_CLEAR_COLOR_RGBA_MASK) == 0)
		return;

	const u32 r = rsx::method_registers.clear_color_r();
	const u32 g = rsx::method_registers.clear_color_g();
	const u32 b = rsx::method_registers.clear_color_b();
	const u32 a = rsx::method_registers.clear_color_a();
	const u32 argb = (a << 24) | (r << 16) | (g << 8) | b; // A8R8G8B8, stored big-endian below
	g_webgpu_clear_color.store(argb, std::memory_order_release); // M1a: publish to main-thread renderer

	const u32 off   = rsx::method_registers.surface_offset(0);
	const u32 pitch = rsx::method_registers.surface_pitch(0);
	const u32 w     = rsx::method_registers.surface_clip_width();
	const u32 h     = rsx::method_registers.surface_clip_height();
	const u32 addr  = rsx::get_address(off, CELL_GCM_LOCATION_LOCAL);
	if (!addr || !w || !h || !pitch)
		return;

	u8* const basep = static_cast<u8*>(vm::base(addr));
	// CUT-1+: force OPAQUE so undrawn areas present as the clear color (not the transparent canvas showing
	// white through). The game clears the menu backbuffer to 0x00000000 (its animated world bg lives in a
	// separate RTT we don't populate); making it opaque black gives the menu UI a clean dark backdrop.
	const u32 fillv = argb | 0xff000000u;
	for (u32 y = 0; y < h; y++)
	{
		be_t<u32>* row = reinterpret_cast<be_t<u32>*>(basep + static_cast<u64>(y) * pitch);
		for (u32 x = 0; x < w; x++)
			row[x] = fillv;
	}

	// === P2 DEV-TEST (2026-06-03): software textured-quad rasterizer — PIPELINE VALIDATION ===
	// Proves the draw→VRAM-surface→flip→WebGPU-blit→canvas path renders REAL per-pixel geometry
	// (a textured quad), not just a flat clear. A full-screen splash IS an axis-aligned textured
	// quad, so this axis-aligned textured-quad rasterizer is the actual reusable splash renderer —
	// here fed a synthetic UV-gradient+grid texture (clearly a dev pattern, NOT the splash) to
	// validate it. When P1 lands (game emits the real splash draw), this gets fed the real GTF
	// texture + the draw's quad coords instead of the synthetic test inputs. Toggle with s_p2test.
	{
		static const bool s_p2test = false; // dev-test OFF: the real-draw path in end() is now the renderer;
		                                    // surface reflects the honest (P1-gated, black) game state.
		if (s_p2test)
		{
			// synthetic 64x64-ish procedural texture sampler: red(x)/green(y) gradient + white grid.
			const auto texsample = [](float u, float v) -> u32 {
				int tx = static_cast<int>(u * 64.0f) & 63;
				int ty = static_cast<int>(v * 64.0f) & 63;
				u32 r = static_cast<u32>(tx * 4), g = static_cast<u32>(ty * 4), b = 64u;
				if ((tx & 7) == 0 || (ty & 7) == 0) { r = g = b = 255u; } // grid lines
				return 0xff000000u | (r << 16) | (g << 8) | b;            // ARGB
			};
			// centered axis-aligned quad over the middle half of the surface, with linear UVs.
			const float qx0 = w * 0.25f, qy0 = h * 0.25f, qx1 = w * 0.75f, qy1 = h * 0.75f;
			const int minx = static_cast<int>(qx0), maxx = static_cast<int>(qx1);
			const int miny = static_cast<int>(qy0), maxy = static_cast<int>(qy1);
			for (int y = miny; y < maxy && y < static_cast<int>(h); y++)
			{
				be_t<u32>* row = reinterpret_cast<be_t<u32>*>(basep + static_cast<u64>(y) * pitch);
				const float vv = (static_cast<float>(y) - qy0) / (qy1 - qy0);
				for (int x = minx; x < maxx && x < static_cast<int>(w); x++)
				{
					const float uu = (static_cast<float>(x) - qx0) / (qx1 - qx0);
					row[x] = texsample(uu, vv);
				}
			}
			static thread_local u32 s_pt = 0;
			if (s_pt++ < 3u)
				printf("[P2-DEVTEST] rasterized synthetic textured quad into surface %ux%u @0x%x (pipeline validation, NOT the splash)\n", w, h, addr);
		}
	}

	static thread_local u32 s_clr = 0;
	if ((s_clr++ & 0x3Fu) == 0)
		printf("[WGPU-CLEAR] argb=0x%08x %ux%u off=0x%x pitch=%u addr=0x%x\n", argb, w, h, off, pitch, addr);
#endif
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

	// M2a: pair this frame's captured texture with the new flip count (or clear it on a clear-only frame).
	{
		const u64 next = g_webgpu_flip_count.load(std::memory_order_relaxed) + 1;
		if (s_frame_has_tex)
		{
			g_webgpu_tex_host.store(s_frame_tex_host, std::memory_order_relaxed);
			g_webgpu_tex_w.store(s_frame_tex_w, std::memory_order_relaxed);
			g_webgpu_tex_h.store(s_frame_tex_h, std::memory_order_relaxed);
			g_webgpu_tex_pitch.store(s_frame_tex_pitch, std::memory_order_relaxed);
			g_webgpu_tex_flip.store(next, std::memory_order_release);
		}
		else { g_webgpu_tex_flip.store(0, std::memory_order_release); }
		s_frame_has_tex = false;
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
