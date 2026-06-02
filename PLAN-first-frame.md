# Plan — Reach Terraria's First Frame (505 splash) on the WebGPU/wasm RPCS3 POC

---

## ⭐⭐ SESSION 3 BREAKTHROUGH (2026-06-03) — the game RUNS its render loop now

From "deadlocked, 0 frames" to **running frames continuously** (`flip n=1,2,3,4…`, 152
ConvertAndFlips, `GSH blit=1 flip=152`, no crash). What landed:

1. **idx6 decoded = a per-frame FLIP-COMPLETE barrier** (via full struct dump `[BARRIER+]`): a
   RenderLoop-owned C++ sync object (`+0x0` vtable, `+0x14`=RenderLoop tid, sems idx4 `+0xe4` /
   idx6 `+0x114`). main does `post idx5 (render-go) → wait idx6 (flip-done)` each frame.
2. **Frame loop SUSTAINED** — `cellResc.cpp::cellRescSetConvertAndFlip`: the game's RESC flip
   handler (`cellRescSetFlipHandler → 0x599f38`) never ran because the raw `GCM_FLIP_COMMAND`
   injection bypasses the flip-notification path. **Now invoke it directly on the RenderLoop PPU
   thread** (`render->flip_handler(ppu, 0)`) → it posts main's idx6 sem AND sets the RenderLoop's
   flip-done flag → both unblock → the loop runs. (+ belt-and-braces hard-post of idx6.)
3. **RSX empty/degenerate-draw crash FIXED** — `draw_call.hpp::get_range()`: once main drives
   frames, the RESC path submits degenerate begin/end (empty OR out-of-bounds `draw_command_ranges`);
   upstream `ensure()` killed the RSX thread → no flips. Guarded to return a zero range (skip).
   ⚠️ **HEADER change** — must `touch RSXThread.cpp Core/RSXDrawCommands.cpp ui_js/WebGPUGSRender.cpp
   rsx_methods.cpp` before rebuild or ninja won't recompile it (cost me a cycle).
4. **SPU group-start + SPURS kernel HLE-dispatch** — `cellSpurs.cpp::cellSpursWakeUp` starts the SPU
   group; `cellSpursSpu.cpp::spurs_hle_dispatch` + `SPUThread.cpp` interpreter hook run the HLE
   kernel (entry 0x818/0x848, exit 0x808/0x838, workload-entry 0xA00 sys-srv|taskset, syscall 0xA70).
   SPU kernels now reach the interpreter. **NB: this is the AUDIO SPURS (`msngSPURS_MP3`)** — it
   fails on the MP3 codec and is NOT the resource gate. Kept as a real capability.

### Current gate (session 3 end): main RUNS but renders an empty scene
- ✅ **Assets fully load now** — `REGCOUNT` reaches **601/601** (was 599; the running render loop
  let the ResourceLoop finish). So the gate is NOT the asset pipeline.
- ✅ main runs its frame loop at ~50fps (post idx5 / wait idx6 / `usleep(20ms)` @0x63c98). The
  usleep loop is per-frame pacing, not a deadlock (main is seen past its exit, cycling).
- ❌ **main issues ZERO `verts>0` draws.** It does only ~8 `end()` calls total — all empty quads
  (`prim=8 verts=0`) — then just presents empty frames. `centerRow nonblack=0` → black canvas.
  main sits in a **steady minimal-render state** (interp cycles `0x418bf0`/`0x63c98`), NOT
  progressing to populate the scene with the splash.
- ⇒ Two gates remain to the 505 splash, both deep:
  1. **Game-logic:** get main to progress from this minimal-render state to actually drawing the
     splash quad (with real vertices/texture). It's waiting on some state/trigger that isn't firing
     — NOT assets (loaded). Needs decoding main's per-frame scene-build at `0x418bf0` to find what
     gates the splash draw.
  2. **Wall B (renderer):** even once main draws a real quad, `WebGPUGSRender::end()` =
     `execute_nop_draw()` — it doesn't rasterize. Need a real RSX→WebGPU (or CPU-raster-into-VRAM)
     backend to turn the quad+texture into pixels. Multi-week.
- NB: a WebGPU "Destroyed texture used in a submit" error appears in GSH (swapchain lifecycle) —
  benign while the buffer is empty, but must be fixed once there are real pixels to present.

### Honest status at session 3 end
The project went from **"deadlocked at a resource poll, 0 PS3 frames"** to **"boots Terraria into a
continuously-running render loop at ~50fps with all 601 assets loaded."** The defining deadlock is
gone. But the 505 splash specifically is still behind (1) a game-logic gate (main not progressing
to the splash draw) and (2) Wall B (no real renderer). Both are fresh, deep investigations; the
renderer alone is multi-week. This is a real, major milestone — not the splash yet.

---

## ⭐ CURRENT STATE — re-baselined 2026-06-02 (supersedes everything below)

**The old body of this file (and `plan-poc-ps3-webgpu.md`) is STALE.** It describes
main stuck at a `0x27700` resource poll with "every table entry empty / `data.vfs`
never read." That is no longer the failure. Ground truth from a fresh `ram=2048`
boot (`capture_screenshot.js terraria … "ram=2048"`):

- `data.vfs` **is** read; resources register (`[REGCOUNT] reg=599/601`).
- The **EE RenderLoop (`tid 0x1000003`)** reaches the RESC present path and a flip fires.
- Boot survives all init (firmware, threads, trophy, savedata, audio, SPURS-init).

### ✅ Landed this session — RESC present path fixed
`cellRescGcmSurface2RescSrc` (cellResc.cpp) was a **stub returning `CELL_OK` without
writing `rescSrc`**. So the render loop's per-frame cycle
`GcmSurface2RescSrc → cellRescSetSrc → cellRescSetConvertAndFlip` failed at SetSrc
(`CELL_RESC_ERROR_BAD_ARGUMENT`, width/height==0) and never published a display buffer.
Implemented the real conversion (GCM color-surface A → RESC src). Verified:
`GcmSurface2RescSrc … 960x540 … → cellRescSetSrc count=1 → flip n=1 count=1 →
db[0] 960x540 vm_addr=0x40000000 → [GSH] blit=1`. The canvas now blits the game's
**real display buffer** (not the fake animated clear). Diagnostic logs added:
`[RESC] cellRescSetSrc REJECT …`, widened `[SEMPOST]` to the whole `0x9600xxxx` pool.

### 🧱 Wall A — the frame never completes (boot deadlock). IN PROGRESS.
main (`tid 0x1000000`) loops `post 0x96004500 (idx5) → sys_semaphore_wait 0x96004600
(idx6) @lr=0x411150`. **`0x96004600` is NEVER posted by any thread** (verified: 0 posts
across the pool; `[WAITFORCE]` band-aid force-releases main each spin so it livelocks).
- Sem pool = base `0x96004000` + index×0x100. main posts idx5, render posts idx4
  (`0x96004400` @lr=0x421768), audio loop (`tid 0x1000002`) stuck on idx1 (`0x96004100`).
- **DECODED — idx6 is a TASK-COMPLETION BARRIER, not a flip signal.** main's loop at game
  `0x411168` (raw disasm via `[IDX6D]` probe in PPUThread.cpp) is:
  `counter=*(r31+0x108); counter++; store; r3=*(r31+0x114)=0x96004600; r4=0; sc(92=sem_wait);
   if ((counter + r30) < *(r31+0x104)=limit) loop`.
  → main blocks until idx6 is posted **`limit` times** = "wait for N dispatched async tasks to
  complete." Those N tasks are the resource-decode jobs main feeds to work-queue `0x96006800`
  (635 posts observed). The per-task completion posts (idx6) NEVER arrive, so each wait relies
  on the 9 s WAITFORCE timeout → main crawls forever. **This is the commit-message
  "SPU-processed assets never register" wall, seen from the consumer side.**
- **Barrier-skip experiment** (`sys_semaphore.cpp`, `#ifdef __EMSCRIPTEN__`: short-circuit ONLY
  main's `0x96004600` waits) → main **exits the barrier and advances** — but to the NEXT
  async-gated wait: a tight `cellKbGetInfo + sys_timer_usleep(20ms)` poll loop (game `0x63c98`,
  HLE `0x9c6f54/0x9c6f74`). No crash. **But `end()` is STILL never called — 0 RSX draws.**
  ⇒ Forcing past barriers is whack-a-mole; the render path never issues scene draws because it
  too is gated on the (incomplete) resource pipeline.
- **ROOT CAUSE (conclusive): the async resource-decode pipeline never completes.** main + worker
  threads (`0x1000002` audio, `0x1000006` work-queue consumer) + SPURS SPU kernels form a
  producer/consumer chain whose terminal step is SPU/SPURS task execution that doesn't run to
  completion in this port. Every main wait (resource-poll `0x27700`, idx6 barrier, kb-poll) is
  downstream of it, and the game won't issue *any* draws without the decoded assets.
- **Real fix for Wall A is NOT patching sync primitives** — it's making the decode pipeline
  complete: (a) get the SPU/SPURS decode tasks actually executing to completion, OR (b) HLE the
  specific decode + resource-registration in C++ (compiles to fast wasm) so resources register
  and the completion sems fire naturally. Only then will main advance AND issue draws.
- (The RSXThread.cpp `flip_handler` dispatch from attempt #1 is committed but inert for this
  `isHLE=0` title — wrong mechanism, left as a no-op. `_gcm_intr_thread`'s "Unimplemented
  syscall_972" is a red herring: sys_dbg_*, `null_func`/`CELL_OK` upstream.)

### 🧱 Wall B — empty render target (the fundamental one). NOT STARTED.
`WebGPUGSRender` is Null-style: `end()=execute_nop_draw()`, and the present path **reads
pixels straight out of guest VRAM** (`g_webgpu_fb_addr`). The probe shows the published
960×540 buffer is **all zeros** (`centerRow nonblack=0`). Terraria's splash is GPU-drawn,
so VRAM stays black → **blit-from-VRAM (old plan "Option 1") cannot work for this title.**
Real pixels require translating RSX draws → WebGPU (surface cache, vertex/texture upload,
RSX FP/VP → WGSL). This is the plan's XL item and gates the actual splash regardless of Wall A.

### Order of attack (revised after the decode)
The decode changes the picture: **Wall A is not a sync-primitive bug** — it bottoms out at the
async resource-decode pipeline (SPU/SPURS), and the game issues **0 draws** until assets decode.
So the real sequence is:
1. **Wall A = resource pipeline.** Make the decode complete — either (a) get SPU/SPURS actually
   executing the decode tasks to completion (multi-day: the SPURS PPU handler is disabled in
   `cellSpurs.cpp` and the SPU-side kernel is partial), or (b) identify the decode codec
   (`data.vfs` → in-EBOOT inflate-HLE `@0x41c294` + any SPU codec) and HLE it in C++ so resources
   register and the completion sems fire naturally. Verify: main advances past the idx6 barrier
   on its own AND `WebGPUGSRender::end()` starts firing (draws issued).
2. **Wall B = renderer.** Once draws are issued, build a minimal RSX→WebGPU backend (surface
   cache + vertex/texture upload + RSX FP/VP→WGSL), or a CPU-rasterizer-into-VRAM for the 2D
   subset, until the 505 splash appears.

Both are multi-week. **Honest odds:** this is the SPU-wall the original plan flagged as the
"pivot to JIT/AOT" trigger. The PPU/SPU *interpreters* may be too slow to decode `data.vfs`
in acceptable time even if the pipeline is wired — which is why the original plan deferred
playable framerate to the wasm-JIT/AOT track. A C++ HLE of the decode (option b) is the most
promising path to a *static* splash specifically, since it sidesteps SPU-interpreter speed.

### SPU root-cause chain (session 2 — 2026-06-03, the deepest dig)
Decoded the async-completion wall all the way down:
- **main's gate is a `limit=1` barrier**, not "wait for 601 resources." `[BARRIER]
  r31=0x35ef0c00 limit(+0x104)=1 counter(+0x108)=1 sem=0x96004600 r30=0`. main does a
  *sequence* of single-completion waits (game loop @0x411168), each blocking on ONE idx6
  post that never comes. The producer posts **nothing** (workers post 0 sems).
- **SPU threads never executed.** `[SPURUN]/[SPUENTRY]/[SPUPROG]` were all 0 — the SPURS
  SPU kernels (`audiCellSpursKernel0-3`) spawned but never reached the interpreter.
- **Root: the SPU thread group is never started.** `sys_spu_thread_group_start` count=0.
  It's only called from `_spurs::handler_entry` (cellSpurs.cpp:729), spawned by
  `_spurs::create_handler` — which is **commented out** (lines 761/772-777).
- **FIX LANDED (cellSpurs.cpp `cellSpursWakeUp`, `#ifdef __EMSCRIPTEN__`): start the group
  directly when SPURS is woken.** Verified: `[SPURSSTART] …group_start(0x4000100)=0` and
  `[SPURUN]/[SPUENTRY]` now fire — **the SPU kernels reach the interpreter for the first time.**
- **Next wall: the SPURS *kernel* isn't wired.** SPU sits at `pc=0x818 op=0x00000000` (stop)
  forever — LS has no kernel code. The kernel is HLE'd (`spursKernelEntry` in cellSpursSpu.cpp,
  EXISTS) but invoked via `spu.RegisterHleFunction(entry, spursKernelEntry)` — a **defunct API**
  (commented at cellSpurs.cpp:1297 + cellSpursSpu.cpp's own registration lines), AND the **wasm
  SPU interpreter has no HLE-function dispatch** (it just runs LS opcodes: `table.decode(op)`).
  The kernel's atomic `vm::reservation_op`s are also commented throughout cellSpursSpu.cpp.
- **To finish Wall A's SPU path:** (1) add an HLE-function registry + dispatch to the wasm SPU
  interpreter (SPUThread.cpp cpu_task), (2) re-enable the `spursKernelEntry`/exit/select
  registrations, (3) provide non-atomic equivalents for the commented `reservation_op`s,
  (4) ensure SPU channel/DMA works for the kernel + the dispatched decode task. Then the decode
  task runs → resources complete → completion sems post → main's barriers clear.
- **CAVEAT:** a prior note in cellSpursSpu.cpp says "SPURS is not main's direct boot-poll
  producer" — so the idx6 producer for main's *current* barrier may be a different thread, not
  the audio SPURS. The idx6 producer is still unidentified (never posts; 0 SEMPOSTs). Confirm
  whether the SPU pipeline actually feeds main's idx6, or trace the idx6 producer separately.

### Repro / build (verified working this session)
```sh
# edit under rpcs3/Emu/** or Utilities/** → MUST rebuild emucore first:
source /Users/bank23232525/emsdk/emsdk_env.sh
cmake --build build_cmake/build/root-wasm --target rpcs3_emu && ./build_wasm.sh 2048
# run (servers already up: npm@3030, python@8008 in ~/Documents/works/emu/rom):
cd js/rpcs3_browser && HEADLESS=1 node capture_screenshot.js terraria out.png "ram=2048" "<trigger>" "" <delayMs>
# ground-truth signals: [REGCOUNT], [RESC], [WebGPUGSRender] flip n=, centerRow, [GSH] blit=, [SEMBLOCK]/[SEMPOST]
```
⚠️ Use the **2048** variant — the 1024 build is stale (May 28). Headless WebGPU is flaky
(~16k frames then CDP drops; the script auto-reconnects). RESC/deadlock appear ~line 5300.

---

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
