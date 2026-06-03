# PLAN — Port RPCS3's Vulkan renderer to WebGPU

**Question:** instead of the software `P2-DRAW` blit hack, can we port RPCS3's real Vulkan
renderer (`VKGSRender`) to WebGPU and get *real* RSX rendering (geometry, shaders, textures,
blending)?

**Verdict: Yes — and it's the right long-term path.** Vulkan→WebGPU is one of the most natural
GPU-API ports (both are modern *explicit* APIs: command encoders, pipelines, bind groups, render
passes). It's a large, incremental effort, but the architecture is already heavily in our favor.

> ⚠️ **Scope reality:** this is the renderer. It is **independent of the current menu gate** (main
> null-loops in *game logic* before it issues menu draws — see PLAN-first-frame.md). A perfect
> WebGPU backend won't show the Terraria menu until that game-logic corruption is fixed. What it
> *does* do: turn today's fake full-screen-texture splash blit into **real RSX-driven rendering**,
> and unlock every future scene. Treat it as a **parallel track**, not a fix for the null-loop.

---

## 1. Why this is feasible (the architecture is on our side)

RPCS3's renderer is split into a huge **backend-agnostic core** and a thin **backend**:

| Layer | Lines | Port effort |
|---|---|---|
| `rsx::thread` / `GSRender` base (RSX FIFO, method registers, draw clause) | ~22k (`RSX/.`) | **0 — reuse as-is** |
| `RSX/Common/` (surface store, texture-cache templates, vertex layout) | ~13k | **~0 — reuse, templates over backend types** |
| `RSX/Program/` shader decompiler **front-end** (RSX bytecode → AST) | ~8k | **0 — reuse; only the emitter is backend-specific** |
| `RSX/Core/` | ~1.5k | reuse |
| **`RSX/VK/` Vulkan backend** | **~22k** | **the reference to port** (logic translates; API calls change) |

Three things already done for us:

1. **`WebGPUGSRender` is already a `GSRender` subclass** (`rpcs3/ui_js/WebGPUGSRender.h:29`),
   exactly like `VKGSRender`. Same virtual contract — we just fill in more overrides.
2. **The build already targets Dawn's C++ WebGPU API** (`#include <webgpu/webgpu_cpp.h>`,
   `wgpu::Device/Queue/RenderPipeline/Texture/Buffer`). `GSH_WebGPUJs.h:16-23` *already documents*
   the Vulkan→WebGPU object mapping. This API is structurally ~isomorphic to Vulkan.
3. The current renderer already reaches into `rsx::method_registers`, `current_draw_clause`,
   `fragment_textures[]`, `rsx::get_address(...)` — i.e. the RSX front-end is **already linked and
   working** in the WASM build.

### The backend contract (what `VKGSRender` overrides — and we must)
From `VKGSRender.h`:
```
clear_surface(mask)      begin()            end()             emit_geometry(sub_index)   // ← the draw
on_init_thread()         on_exit()          flip(info)        scaled_image_from_memory() // ← cellResc/2D blit
on_access_violation()    on_invalidate_memory_range()         // texture/surface cache protection
begin/end_occlusion_query, conditional_rendering, sync_hint, write_barrier   // can stub first
```
The **heart** is `begin / end / emit_geometry / clear_surface / flip`. Everything else can be
stubbed initially.

### Vulkan ↔ WebGPU concept map (mechanical translation)
| Vulkan (`vk::`) | WebGPU (`wgpu::`) | Notes |
|---|---|---|
| `VkCommandBuffer` | `CommandEncoder` | per-frame |
| `vkCmdBeginRenderPass` | `CommandEncoder.BeginRenderPass` | load/store ops match |
| `VkPipeline` (graphics) | `RenderPipeline` | immutable; cache by state-hash (reuse `ProgramStateCache`) |
| `VkDescriptorSet` | `BindGroup` (+`BindGroupLayout`) | WebGPU caps 4 groups; plan the layout |
| push constants | small uniform buffer / `Queue.WriteBuffer` | WebGPU has no push constants |
| `VkBuffer` + `VkDeviceMemory` | `Buffer` (mappedAtCreation / `WriteBuffer`) | Dawn manages memory |
| `VkImage` / `VkImageView` | `Texture` / `TextureView` | |
| `VkSampler` | `Sampler` | |
| SPIR-V module | **WGSL** `ShaderModule` | ⚠️ **the one real divergence — see §2** |
| `vkCmdDraw[Indexed]` | `RenderPassEncoder.Draw[Indexed]` | |
| compute (format conv, swizzle) | `ComputePipeline` | WebGPU has compute — reuse `VKCompute` logic |

So for most of the 22k lines the port is: **keep the logic, swap `vk::X` → `wgpu::X`.**

---

## 2. The hard parts (honest risk register)

### 2.1 Shader translation → WGSL  *(biggest risk)*
The VK path is RSX bytecode → AST → **GLSL** (`Program/GLSLCommon.cpp`, `VK/VKFragmentProgram.cpp`,
`VKVertexProgram.cpp`) → SPIR-V via glslang → Vulkan. **Browser WebGPU only accepts WGSL.** Options:

- **(A) Per-program RSX→WGSL emitter** — write a WGSL emitter parallel to `GLSLCommon`/the VK FP/VP
  classes. Cleanest, fastest at runtime, but a real compiler effort and WGSL is stricter than GLSL
  (explicit types, no implicit conversions, different texture/sampler binding syntax, no `discard`
  in the same form, `ptr` rules).
- **(B) ✅ Port the *shader interpreter* (uber-shader) — RECOMMENDED FIRST.** RPCS3 has
  `VK/VKShaderInterpreter` (`class shader_interpreter`, 822 lines): **one** über-shader that reads
  RSX shader bytecode from a buffer and *interprets* it on the GPU — no per-program compilation.
  Porting this means hand-writing **one** large-but-finite WGSL interpreter shader. It sidesteps the
  RSX→WGSL *compiler* entirely. Slower per-pixel, but correctness-first and massively de-risks the
  shader problem. RPCS3 ships with this mode usable as the default.
- **(C) GLSL→SPIR-V→WGSL via glslang+Tint** at runtime. Reuses the existing GLSL emitter, but pulls
  glslang **and** Tint into the WASM binary (heavy: size + compile time) and adds a runtime
  translation step. Possible fallback; not the starting point.

**Plan: ship (B) to get real shaders working, then add (A) per-program WGSL for performance.**

### 2.2 PS3 texture formats / swizzle / tiling
PS3 textures are swizzled/tiled and use formats WebGPU doesn't have natively. VK already converts
these (CPU unswizzle + `VKCompute`/copy). WebGPU **has compute and copies**, so the same strategy
ports — but each format path is work. Start with linear `A8R8G8B8` (what the splash uses) and grow.

### 2.3 WebGPU strictness & feature gaps
- No geometry shaders (RSX doesn't need them ✅). No dual-source blending in some impls (check).
- Strict validation (no OOB reads, fixed bind-group counts, alignment rules) — stricter than Vulkan;
  expect to fix latent assumptions the VK code gets away with.
- Limits: max bind groups (4), uniform buffer binding size, `maxColorAttachments` (RSX MRT up to 4 ✅),
  texture-format-specific usage flags. Design the bind-group layout up front.
- `mappedAtCreation` / async buffer mapping ergonomics differ from VK's persistent maps.

### 2.4 Build & runtime
- Confirm `RSX/Common/` + `RSX/Program/` actually **compile into the WASM `emucore`** (they may be
  behind `WITH_VULKAN`/`WITH_GL` guards today — see M0). The base `rsx::thread` is already linked.
- Binary size grows; keep `-Oz` variants building.
- **Performance:** the GPU work runs on the real device via WebGPU (fast). The cost is RSX command
  processing + state hashing + cache lookups on the WASM CPU — but that's the same code VK runs and
  is dwarfed by our PPU/SPU **interpreter** bottleneck anyway.

---

## 3. Incremental milestones (each independently demoable)

> Strategy: **strangle the `P2-DRAW` hack** — replace it surface-by-surface with real WebGPU, so the
> splashes keep rendering throughout and become *real-geometry* rendered, not blitted.

- **M0 — Wire the shared core into the WASM build.** Ensure `RSX/Common/` + `RSX/Program/`
  (decompiler front-end, `ProgramStateCache`) compile + link in `ui_js`. No behavior change.
  *Exit:* builds clean; `ProgramStateCache` instantiable.
- **M1 — Real color surface + clear.** Back the RSX color surface with a `wgpu::Texture` (not a guest
  VRAM memset). Implement `clear_surface` as a real render-pass load-clear. Keep `flip` blitting that
  texture to canvas. *Exit:* clear color shows via a real WebGPU render pass.
- **M2 — `emit_geometry`, untextured.** Upload the draw clause's vertices to a `wgpu::Buffer`, build
  one hardcoded `RenderPipeline` (position + vertex color, no sampling), `Draw`. Render the game's
  **actual triangles** for the first time. *Exit:* a real game draw call produces real geometry.
- **M3 — WGSL shader interpreter (uber-shader).** Port `shader_interpreter` to one WGSL module fed by
  RSX FP/VP bytecode buffers + the existing decompiler front-end's metadata. *Exit:* real RSX
  fragment/vertex programs run on the GPU; the splash renders through the *real* shader path.
- **M4 — Texture cache.** Port `VKTextureCache`/`VKTexture` (start: linear `A8R8G8B8`), sampler
  state, bind groups. *Exit:* sampled textures via real samplers (the splash texture, properly).
- **M5 — Surface cache + full state.** Port `VKRenderTargets` (surface store, MRT, depth/stencil),
  blending, viewport/scissor, the `method_registers` → pipeline-state mapping. *Exit:* multi-pass /
  blended scenes.
- **M6 — Present + 2D blit.** Fold `VKPresent` logic into the existing `GSH_WebGPUJs` swapchain;
  implement `scaled_image_from_memory` (the cellResc path) as a real blit. *Exit:* `flip` and
  cellResc go through the real backend; delete `P2-DRAW`.
- **M7+ — Per-program RSX→WGSL (perf), more formats, occlusion queries, ZCULL, compute conversions.**

Each milestone maps to a VK reference file:

| Milestone | Port from |
|---|---|
| M1/M5 surfaces | `VKRenderTargets.{h,cpp}`, `VKRenderPass.cpp` |
| M2/M3 draw+pipeline | `VKDraw.cpp`, `VKGSRender.cpp::emit_geometry`, `VKProgramPipeline.cpp` |
| M3 shaders | `VKShaderInterpreter.cpp`, `VKFragmentProgram.cpp`, `VKVertexProgram.cpp`, `Program/GLSLCommon.cpp` |
| M4 textures | `VKTextureCache.{h,cpp}`, `VKTexture.cpp`, `VKFormats.cpp` |
| M6 present/blit | `VKPresent.cpp`, `VKResolveHelper.*`, `scaled_image_from_memory` |
| M7 compute | `VKCompute.{h,cpp}`, `VKDMA.cpp` |

---

## 4. Recommended first move: a **Phase-0 spike** (1–2 days, validates the riskiest assumptions)

Before committing to the milestone march, prove the two unknowns in a throwaway branch:

1. **Dawn-in-emscripten WGSL pipeline works end-to-end:** hand-write a trivial WGSL vertex+fragment
   shader, build a `wgpu::RenderPipeline`, draw one textured triangle into the existing surface, and
   see it on canvas. (Confirms our Dawn build accepts WGSL, bind groups, and a real render pass —
   *not* just the surface clear it does today.)
2. **Uber-shader feasibility in WGSL:** prototype a tiny "interpreter" WGSL shader that reads a few
   opcodes from a uniform/storage buffer and branches — confirms WGSL's dynamic indexing/loop limits
   are acceptable for porting `shader_interpreter` (M3).

If both pass, the rest is (large but) mechanical translation. If WGSL dynamic-shader limits bite,
fall back to per-program RSX→WGSL (option A) or the glslang+Tint path (option C).

---

## 5. Decision summary

- **Do it?** Yes, if the goal is real rendering beyond splashes — there is no other path (the
  `P2-DRAW` blit fundamentally can't do geometry/shaders/blending).
- **Effort:** multi-month, incremental. ~22k lines of VK to translate, mostly mechanical, atop ~45k
  lines reused for free. The shader/WGSL piece is the real research risk — de-risked by the
  uber-shader (M3 / option B).
- **Sequencing vs the menu:** parallel track. It makes the *existing* (splash) rendering real and
  unlocks future scenes, but the **menu still needs the game-logic null-loop fixed first**
  (the stray-store/stack-corruption hunt in PLAN-first-frame.md).
- **Lowest-regret next step:** the §4 Phase-0 spike — cheap, and it converts "can we?" into "yes,
  here's the pipeline" or "here's the exact wall," before any big commitment.

---
*Refs: `rpcs3/ui_js/WebGPUGSRender.{h,cpp}`, `GSH_WebGPUJs.{h,cpp}`; `rpcs3/Emu/RSX/VK/*`;
`rpcs3/Emu/RSX/Program/*`; `rpcs3/Emu/RSX/GSRender.h`, `thread.h`. Created 2026-06-03.*
