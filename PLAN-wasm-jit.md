# Plan — WASM JIT / AOT recompiler for the browser RPCS3 POC

## Why
The wasm build runs the **static PPU & SPU interpreters** (~1–3M instr/s) because RPCS3's
native JITs (LLVM→x86/ARM, ASMJIT→x86/ARM) **cannot run in a browser sandbox** (no executable
native pages). This is the root cause of: slow boot, and SPURS real-time deadlines being missed.
Goal of this track: replace interpretation with **wasm code generation** to get 1–2 orders of
magnitude speedup → fast boot + a shot at past the SPURS speed wall (and, longer-term, framerate).

NOTE: NOT required for the *static* first-frame/splash (see PLAN-first-frame.md) **unless** that
effort bottoms out at a SPU/SPURS-task producer — then SPU codegen (Phase 3) becomes the path.

## How RPCS3's recompilers are wired today (reuse audit)
- PPU: `PPUTranslator` (Emu/Cell/PPUTranslator.cpp) emits **LLVM IR**; `jit_compiler`
  (Utilities/JIT.cpp, LLVM ORC/MCJIT) compiles IR → **native**. Dispatch in PPUThread.cpp.
- SPU: `spu_recompiler_base`/LLVM (Emu/Cell/SPURecompiler.cpp) emits IR→native, and
  `spu_recompiler` ASMJIT (SPUASMJITRecompiler.cpp) emits native **directly** (no IR).
- Interpreters: `ppu_interpreter`, `spu_interpreter` (the wasm fallback).
- AOT cache already exists conceptually: the "Compiling PPU/SPU modules" precompile → cached `.obj`.
- LLVM **has** a `wasm32` backend. Browser **has** runtime `WebAssembly.compile/instantiate` +
  growable indirect `Table` (emscripten `addFunction`, needs `-sALLOW_TABLE_GROWTH`).

## Two approaches (and the recommended hybrid)
### Approach 1 — Runtime wasm-JIT (true dynarec, emit wasm at runtime)
Per hot block: guest code → wasm bytecode → instantiate → add to indirect table → `call_indirect`.
- 1a. via LLVM in-browser: **impractical** (LLVM-compiled-to-wasm is hundreds of MB + slow).
- 1b. **hand-written wasm emitter** backend (opcode → wasm template, assemble module/block):
  the realistic "true JIT". Big new component. Issues: per-block compile latency, thousands of
  modules/table entries (browser caps), module eviction/GC, JS↔wasm boundary.

### Approach 2 — AOT (recompile offline to wasm, load in browser)  ← RECOMMENDED FIRST
Run the recompiler **offline on a real machine** (LLVM works): guest code → LLVM IR →
LLVM `wasm32` backend → `.wasm` module(s) + a guest-PC→func dispatch map. Browser loads at
startup and dispatches into them. **Sidesteps LLVM-in-browser entirely.**
- Pro: reuses PPUTranslator's existing IR; translation runs on hardware; no runtime codegen.
- Con: can't cover **runtime-only code** — SPU overlays / SPURS jobs streamed from `data.vfs`
  are unknown offline → those still need interpreter (or a small runtime JIT) fallback.

### Recommended: HYBRID
AOT the static/firmware code (PPU + liblv2 + **libsre SPURS kernel**) for the bulk speedup;
keep the interpreter (or add a minimal runtime SPU JIT later) for dynamic SPU jobs.

## Phased plan
### Phase 0 — Feasibility spike & decision (~1–2 wk)
- [0.1] Audit JIT.cpp / PPUTranslator / SPU recompilers; map the IR→native seam where wasm
  would substitute.
- [0.2] Spike: get LLVM (offline) to emit a `wasm32` module for ONE trivial PPU function;
  load it in a tiny browser harness and call it. Proves the toolchain end-to-end.
- [0.3] Baseline: measure interpreter instr/s; estimate AOT speedup target.
- [0.4] DECISION: confirm AOT-first hybrid (vs pivot).

### Phase 1 — Minimal end-to-end AOT (prove pipeline) (~1–2 wk)
- [1.1] Offline: translate ONE proven hot PPU loop (e.g. the inflate match-copy @0x41c294, or
  the 88MB arena-init @0x33f24) → wasm module.
- [1.2] Browser: dispatch that guest PC to the AOT func; verify **bit-exact** result + speedup.
- [1.3] Build the loader + guest-PC→func table + interpreter fallback seam.

### Phase 2 — AOT the static PPU code (EBOOT + firmware) (~weeks)
- [2.1] Offline tool: drive PPUTranslator with `wasm32` target over EBOOT + liblv2/libsysmodule/
  etc.; emit wasm module(s) + dispatch map; handle relocations/guest-addr mapping.
- [2.2] Browser dispatch: "if AOT func for PC exists → call_indirect; else interpret."
- [2.3] Wire guest memory: generated wasm imports the shared linear memory (the 1.5GB vm region).
- [2.4] Verify boot correctness + measure boot-time drop. Threading: shared modules, per-thread state.

### Phase 3 — SPU codegen + the dynamic-code problem (~month+)  ← the hard + high-value part
- [3.1] AOT the **libsre SPURS kernel** SPU image (static, known offline).
- [3.2] Dynamic SPU jobs (streamed from data.vfs): choose
  (a) interpreter fallback (slow, simplest),
  (b) a minimal **runtime SPU wasm-JIT** (Approach 1b, scoped to SPU blocks), or
  (c) AOT a fixed/known job set.
- [3.3] Goal: SPURS real-time deadlines met → unblocks the asset pipeline / framerate.

### Phase 4 — General runtime wasm-JIT (optional) (~month+)
Only if AOT coverage is insufficient. Hand-written wasm-emitter backend, per-block
compile/instantiate/table-add + eviction + cache.

### Phase 5 — Integration, perf tuning, threading correctness, regression tests.

## Key components to build
1. **Offline AOT tool** — RPCS3 build that drives the recompiler with `wasm32` target, outputs
   `.wasm` + dispatch map (extends the existing PPU/SPU precompile cache).
2. **Browser AOT loader** — fetch/instantiate modules, import shared memory + runtime helpers,
   register exports into the indirect table.
3. **Dispatch seam** — guest-PC → (AOT func | interpreter); in PPU/SPU thread loops.
4. **(Phase 4) runtime wasm emitter** — opcode → wasm bytecode templates.

## Risks / unknowns / decision points
- **Dynamic SPU code** (SPURS jobs from assets) is the crux — pure AOT can't see it. Likely
  needs interpreter fallback or a runtime SPU JIT → that's the long pole.
- **LLVM wasm32 output quirks** (calling convention, indirect calls, guest-mem access patterns,
  exceptions/longjmp via Asyncify) — spike in Phase 0.
- **Browser limits**: module count, `Table` growth, instantiation latency, 4GB memory cap.
- **Self-modifying / overlay code** invalidation.
- **Maintenance**: an offline tool + browser loader is a permanent new subsystem.

## Honest effort estimate
Multi-month, research-grade. Phase 0–1 spike ~2–4 wk (this is where to validate go/no-go).
Phase 2 (PPU AOT, fast boot) is the first big payoff. Phase 3 (SPU/dynamic) is the hard,
high-value milestone that could actually beat the SPURS wall.

## Relationship to first-frame goal
- First-frame plan can likely reach the **static splash** without this (PPU-side fixes).
- If first-frame hits a SPU/SPURS-task wall → jump to **Phase 3 (SPU codegen)** here.
- This track is what's needed for any move toward **framerate** (still not "playable" without
  substantial Phase 3+ success, but qualitatively different from the interpreter).
