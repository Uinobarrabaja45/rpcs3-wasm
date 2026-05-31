# Plan — Reach Terraria's First Frame (505 splash) on the WebGPU/wasm RPCS3 POC

## Definition of done
A screenshot showing the **real 505 Games splash** (orange "505" + gray "GAMES" on black,
matching `js/rpcs3_browser/public/terraria.png`), produced by the RSX executing real GCM
commands → `WebGPUGSRender::flip()` → WebGPU blit. **No faked pixels.**
Success signal: `[WebGPUGSRender] flip n=1` fires (first real flip) and the blit shows the splash.

Non-goal: playable framerate (NOT achievable on the static PPU/SPU interpreters — that needs
the wasm-JIT/AOT track, deferred).

## The dependency chain (what must happen, reverse-ordered from the flip)
1. `flip()` runs ⟸ RSX RenderLoop processes a flip command  *(pipeline already ALIVE)*
2. main issues an RSX flip (builds a GCM command buffer w/ a draw, calls cellGcmSetFlip)
3. main passes its resource-poll at `0x27700` ⟸ resource ID it wants is registered
4. the resource/handle table (0x68c-stride, ~390 entries @ ~0x8axxxxx) gets entries
   registered (entry `+0x60` halfword set from `0xffff` → real ID) ⟸
5. the asset loader reads `data.vfs` → decompresses → registers resources ⟸
6. the asset-loader thread (WorkerThread/ResourceLoop) unblocks after its init  ← **CURRENT GATE**

## Current state (≈stackwalk147)
- Boot survives deep init: firmware, threads, trophy×2, savedata, audio, SPURS-init. ✅
- vblank pipeline alive; `WebGPUGSRender::flip()` blit implemented. ✅
- main blocks at `0x27700` busy-poll: searches the table for a resource ID (e.g. `0x0191`),
  but every entry's `+0x60` is `0xffff` (empty). `sys_fs_read`=0 → data.vfs never read.
- SPURS is NOT main's direct blocker (keeping the kernel alive didn't help).
- Asset loader goes quiet after trophy/savedata/userinfo init — block type TBD (WORKERCIA probe).

## Phase A — Unblock the asset loader  ← WE ARE HERE
- [A1] WORKERCIA probe: is the loader **spinning** (where?) or **syscall-blocked** (which?).
- [A2] Classify & fix the block:
  - sync wait (cond/sem/event) → find the signaler thread, fix why it never signals
  - busy-poll → trace the upstream producer it waits on
  - `sys_ppu_thread_join` → which thread, why it never exits
  - SPURS-task wait → **speed wall** → fall back to targeted HLE (Phase B fallback)
- [A3] Verify: `sys_fs_read`/`cellFsRead` on **fd=3 (data.vfs)** appears.

## Phase B — Asset load → decompress → register resources
- [B1] Confirm data.vfs reads (fd=3).
- [B2] Confirm decompression runs (in-EBOOT inflate-HLE @ `0x41c294` fires); keep it fast.
- [B3] Confirm resources registered: table entries' `+0x60` set to real IDs → main's poll matches.
- Verify: main's poll target (e.g. `0x0191`) becomes present in the table.
- **Fallback (if registration is SPU/SPURS-driven & too slow):**
  - honest-mark the *specific* resource main waits for (write its `+0x60` = target) **iff the
    resource data is actually loaded** (else it'll use empty data → crash), OR
  - HLE the registration step in C++ (compiles to fast wasm), OR
  - escalate to JIT/AOT track.

## Phase C — main proceeds → RSX render setup → first flip
- [C1] main passes the poll → render init. Watch for `cellGcmInit`, `cellGcmSetFlipMode`,
  `cellGcmSetDisplayBuffer`, command-buffer build, draw calls.
- [C2] main issues the first flip (NV4097 flip / `sys_rsx` flip / cellGcmSetFlip).
- Verify: RSX RenderLoop receives flip; `flip()` called; `g_webgpu_flip_count` bumps.

## Phase D — Blit → splash → capture
- [D1] `flip()` publishes front buffer (g_webgpu_fb_addr/host/w/h/pitch); GSH_WebGPUJs blits
  ARGB-BE → BGRA → swapchain.
- [D2] **Resolution reconcile:** GSH haveFrame check expects `fb_w==1280 && fb_h==720`, but
  CreateVM sets `_480p`. Use the actual display-buffer dims from `flip()` (likely 720×480 or
  the game's chosen mode); relax/fix the 1280×720 gate so the real db blits.
- [D3] Capture (trigger `flip n=`) → screenshot == ref → **DONE**.

## Cross-cutting
- **Host memory:** runs die early under host pressure (other apps). Mitigate: orphan-Chrome
  cleanup in run script (done); accept flaky/re-run; optionally HLE the 88MB arena-init
  (`0x33f24`/`0x1a5730`) to cut ~30s boot + pressure (structs are zero → no-op, skippable).
- **Probes:** keep lean — high-rate printfs flood the CDP console and kill the page.
- Each phase may surface sub-gates; iterate per the loop.

## Risk / honest odds
- Phase A/B tractable **if** the loader block is PPU-logic (missing event, wrong wait, a fixable
  HLE return). **Hard** if it bottoms out at a SPURS-task producer (SPU-speed wall).
- Fallbacks (honest-mark a loaded-but-unmarked resource, or hand-HLE the hot producer) raise the
  odds for a **static splash** specifically — which is the goal — even though they wouldn't help
  framerate.
- If we hit the SPU wall with no honest workaround → that's the signal to pivot to the JIT/AOT track.
