# plan-poc-ps3-webgpu.md — WebGPU PS3 emulator in the browser (POC)

## Goal

Open `http://localhost:3030/?compare=terraria` and have the **left
(Live · WebGPU)** pane render the same frame as the **right (Reference ·
Vulkan)** pane, by actually emulating the PS3 ROM in the browser — not by
uploading the reference image as a placeholder.

Success = the live pane shows the 505 GAMES splash (or whatever the ROM
draws), produced by real PPU/SPU/RSX execution under emcc, presented
through the WebGPU surface that's already wired to `#outputCanvas`.

---

## Core idea (one paragraph)

`main_window::BootISO()` in [rpcs3/rpcs3qt/main_window.cpp:751](rpcs3/rpcs3qt/main_window.cpp#L751)
is the desktop entry point that boots a `.iso`. It does one thing —
calls `Boot(path, "", true, true)`, which in turn calls
[`Emu.BootGame(path, "", true, ...)`](rpcs3/rpcs3qt/main_window.cpp#L580).
*That* call is the seam we need. Expose `Emu.BootGame` over embind,
hand it a path backed by our JS-streamed `fs::file_base`, let the
emucore drive RSX commands into the existing
[`CGSH_WebGPUJs`](rpcs3/ui_js/GSH_WebGPUJs.cpp) surface — done. No new
boot path, no parallel code, just the same `Emu.BootGame` desktop uses.

Build the emucore (no Qt, no Vulkan, no LLVM JIT, no libusb) to WASM via
[`./build_wasm.sh`](build_wasm.sh), drop the artifacts in
[`js/rpcs3_browser/public/`](js/rpcs3_browser/), open the page, watch
the canvas paint.

---

## Current state (already wired)

What works today, after the textured-quad bring-up:

- **React shell** at [`js/rpcs3_browser/`](js/rpcs3_browser/) — splash
  panel, file picker, `?compare=<game>` split view, `?game=<key>`
  auto-fetches a ROM from `localhost:8008`.
- **WebGPU surface** in [`rpcs3/ui_js/GSH_WebGPUJs.cpp`](rpcs3/ui_js/GSH_WebGPUJs.cpp):
  `wgpu::Surface` configured against `#outputCanvas`, `Render()` acquires
  the swap-chain texture, encodes a render pass, submits, lets
  emdawnwebgpu auto-present.
- **Textured fullscreen-quad pipeline** in the same file:
  `EnsureQuadPipeline()` + `DrawReferenceQuad()`. The reference-image
  "cheat" upload (`setReferenceTexture` / `UploadReferenceTexture`) was
  **removed at the user's request** — the POC no longer fakes the live
  pane with `terraria.png`. The pipeline stays so that once RSX command
  playback lands (Phase 5) it can present the real framebuffer.
- **embind surface** in [`rpcs3/ui_js/Main.cpp`](rpcs3/ui_js/Main.cpp):
  `initVmWebGPU`, `bootElf`, `bootDiscImage`, `bootXmb`,
  `setReferenceTexture`, etc. These currently call into
  [`Rpcs3VmJs.cpp`](rpcs3/ui_js/Rpcs3VmJs.cpp) printf stubs — that's
  where `Emu.BootGame` needs to land.
- **Disc-image streaming**: [`DiscImageDevice.ts`](js/rpcs3_browser/src/DiscImageDevice.ts)
  proxies `File.slice()` calls into the WASM heap, so the WASM never
  has to hold a 4 GB ISO. Still needs the C++ `fs::file_base` subclass
  on the other side.
- **Build pipeline**: [`build_wasm.sh`](build_wasm.sh) compiles the
  standalone scaffold to four memory variants and copies to
  `js/rpcs3_browser/{src,public}/Rpcs3{512,1024,2048,Dyna}.{js,wasm}`.
- **Headless screenshot harness**:
  [`capture_screenshot.js`](js/rpcs3_browser/capture_screenshot.js)
  drives a headless Chrome at port 3030, watches console output for a
  boot-trigger string, and saves a PNG.

---

## The gap (what's left to do)

Three threads, roughly independent:

### 1. Wire `Emu.BootGame` into `Rpcs3VmJs::BootDiscImage`

[`Rpcs3VmJs.cpp`](rpcs3/ui_js/Rpcs3VmJs.cpp) is the printf stub today.
The actual work is replacing its body with the same sequence the Qt
`main_window::Boot` does — minus the Qt/dialog/recent-list noise. The
call we want lives at [main_window.cpp:580](rpcs3/rpcs3qt/main_window.cpp#L580):

```cpp
Emu.BootGame(path, /*title_id=*/"", /*direct=*/true, cfg_mode::custom,
             /*config_path=*/"", /*db_config=*/std::nullopt);
```

`Emu` is the global `Emulator` defined in
[`rpcs3/Emu/System.h`](rpcs3/Emu/System.h). The minimum embind
plumbing is:

- `Rpcs3VmJs::CreateVM()` calls `Emu.Init()` (sets up vfs, devices,
  config) — see [System.cpp](rpcs3/Emu/System.cpp).
- `Rpcs3VmJs::BootDiscImage(path)` calls
  `Emu.BootGame(path, "", true)` and returns the `game_boot_result`.
- `Rpcs3VmJs::Pause/Resume/Stop` map to `Emu.Pause()` / `Emu.Resume()`
  / `Emu.Kill()`.
- `IsRunning()` reflects `Emu.IsRunning()` so the React shell can
  detect the boot edge.

The `path` we pass is a virtual one — see thread 2.

### 2. Streaming disc image as an `fs::file_base`

`Emu.BootGame` opens the ISO via [`rpcs3/Loader/disc.cpp`](rpcs3/Loader/disc.cpp)
and [`Loader/ISO.cpp`](rpcs3/Loader/ISO.cpp), both of which sit on top
of [`fs::file_base`](rpcs3/Utilities/File.h). Subclass `fs::file_base`
with `read` / `seek` / `size` implementations that bounce through
`MAIN_THREAD_EM_ASM` to `Module.discImageDevice.read(...)`:

```cpp
class CJsDiscFile : public fs::file_base {
    u64 m_pos = 0;
    u64 m_size;
public:
    explicit CJsDiscFile(u64 size) : m_size(size) {}
    u64 read(void* buf, u64 size) override {
        MAIN_THREAD_EM_ASM({
            Module.discImageDevice.read($0, Number($1), $2);
        }, buf, m_pos, size);
        while (!MAIN_THREAD_EM_ASM_INT({ return Module.discImageDevice.isDone(); }))
            usleep(100);
        m_pos += size;
        return size;
    }
    // ...seek / size / stat etc.
};
```

Mount it at a synthetic path (e.g. `/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN`)
via `fs::file::open` overloads, hand that path to `Emu.BootGame`. PS3
disc decryption (`iso_file_decryption` in `Loader/ISO.cpp`) sits above
`fs::file_base` and works unmodified.

PS3 disc keys: the user supplies the matching `.IRD` via the file
picker (or the React shell prompts for it after reading the disc ID
from the first sector). Same model as native rpcs3.

### 3. Linking the emucore into the WASM build

Today [rpcs3/ui_js/CMakeLists.txt](rpcs3/ui_js/CMakeLists.txt) builds
a standalone executable with just `Main.cpp`, `Rpcs3VmJs.cpp`,
`InputProviderEmscripten.cpp`, `GSH_WebGPUJs.cpp`, and the new Phase 2
`JsDiscBridge.cpp` — no emucore.

**Status: STARTED — ~5 of ~30 build-infra blockers cleared.**

#### Starter foundation already landed (probe via `emcmake cmake -DWITH_LLVM=OFF -DUSE_VULKAN=OFF -DUSE_FAUDIO=OFF -DUSE_DISCORD_RPC=OFF -DUSE_LIBEVDEV=OFF -DUSE_SDL=OFF ...`)

- [CMakeLists.txt:92](CMakeLists.txt#L92) — 64-bit pointer guard bypassed on EMSCRIPTEN (with WARNING). WASM32 has 4-byte pointers; rpcs3 has 64-bit assumptions in places (intrusive bit-stuffing, JIT code-emit). See *32-bit pointer hazard* below.
- [3rdparty/CMakeLists.txt](3rdparty/CMakeLists.txt) — empty `INTERFACE` stubs for: `libusb`, `hidapi`, `opengl` (when not Apple/Android), `ffmpeg`, `glew`, `llvm`, `wolfssl`, `libcurl`, `miniupnpc`, `rtmidi`, `opencv`.
- [3rdparty/ffmpeg/CMakeLists.txt](3rdparty/ffmpeg/CMakeLists.txt) — early `return()` on EMSCRIPTEN; no prebuilt wasm32 ffmpeg ships.

These all keep the native build working (guarded with `if(EMSCRIPTEN)`); on emcc they become empty INTERFACE libraries so the rest of the link line is shape-preserved.

#### Configure-time blockers (cleared)

1. ✅ **Alias name collisions** in [3rdparty/CMakeLists.txt:388+](3rdparty/CMakeLists.txt#L388) — added a parallel `if(EMSCRIPTEN)` alias block that uses the existing INTERFACE stubs.
2. ✅ **Qt6** — guarded out in [rpcs3/CMakeLists.txt:31](rpcs3/CMakeLists.txt#L31).
3. ✅ **rpcs3qt subdirectory** — `add_subdirectory(rpcs3qt)` skipped on EMSCRIPTEN.
4. ✅ **rpcs3_lib + Qt-dependent rpcs3 executable target** — skipped on EMSCRIPTEN; only `add_subdirectory(Emu)` runs.
5. ✅ **3rdparty::llvm missing** — added INTERFACE stub + alias.

Configure now reaches "Generating done" with the command:
```sh
EMSDK=/Users/bank23232525/emsdk EMSDK_NODE=$EMSDK/node/22.16.0_64bit/bin/node \
  emcmake cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DWITH_LLVM=OFF -DUSE_VULKAN=OFF -DUSE_FAUDIO=OFF \
    -DUSE_DISCORD_RPC=OFF -DUSE_LIBEVDEV=OFF -DUSE_SDL=OFF \
    -DBUILD_RPCS3_TESTS=OFF -DBUILD_LLVM=OFF \
    /path/to/rpcs3
```

(takes ~130s — protobuf download dominates).

#### Source-level blockers (header layer — CLEARED)

Stub headers in [`3rdparty/_emscripten_stubs/include/`](3rdparty/_emscripten_stubs/include/) wired into the relevant INTERFACE targets in [3rdparty/CMakeLists.txt](3rdparty/CMakeLists.txt) cover the full symbol surface rpcs3 uses:

| Header | Files | Stub status |
|---|---|---|
| `libusb.h` | 19 | ✅ stubbed (`_emscripten_stubs/include/libusb.h`) — covers ~22 funcs + ~30 constants |
| `SoundTouch.h` | 7 | ✅ stubbed — class shape only |
| `wolfssl/ssl.h` + `coding.h` | 26 | ✅ stubbed — compile passes; **runtime crypto is fake** (BootGame will fail at SELF decryption) |
| `miniupnpc.h` + `upnpcommands.h` + `miniwget.h` | 2 | ✅ stubbed |
| `rtmidi_c.h` | 4 | ✅ stubbed |
| `curl/curl.h` | 1 | ✅ stubbed |
| `execinfo.h` | 1 | ✅ stubbed |

Header layer total: 60 errors → 0. Bodies don't exist yet — link will fail with "undefined symbol" for ~80 functions. Either provide stub `.cpp` impls (mechanical) or `#ifdef __EMSCRIPTEN__` guard the call sites (cleaner). Defer until past the source-level porting blocker below.

#### Source-level blockers (next, the REAL ones)

Past the header layer, the build exposes rpcs3 source assumptions that don't hold on wasm32. Counts from `cmake --build --target rpcs3_emu -- -k 1000`:

| Root cause | Status | Error count | Notes |
|---|---|---|---|
| `static_assert(sizeof(usz) >= 2 * sizeof(u32))` at [Utilities/address_range.h:586](Utilities/address_range.h#L586) | ✅ FIXED — `if constexpr` branch picks 64-bit pack or 32-bit XOR | Was ~1400 | Hash quality degrades on wasm32 (collisions allowed in 32-bit buckets); semantically correct |
| `utils::bless()` (rpcs3/util/bless.hpp) | ✅ FIXED — passthrough cast on wasm | Was ~444 | wasm linear memory model makes the original aliasing-trick unnecessary |
| `utils::get_tsc()` (rpcs3/util/tsc.hpp) | ✅ FIXED — `emscripten_get_now() * 3e6` | Was ~97 | Fake TSC at nominal 3 GHz |
| `utils::trap()` (rpcs3/util/asm.hpp) | ✅ FIXED — `__builtin_trap()` | Was ~91 | wasm `unreachable` instruction |
| `utils::pause()` (rpcs3/util/asm.hpp) | ✅ FIXED — memory-barrier noop | Was ~91 | No spin-loop hint in wasm; workers cooperatively scheduled by browser |
| `static_assert((Align & (Align - 1)) == 0)` at [rpcs3/util/atomic.hpp:1180](rpcs3/util/atomic.hpp#L1180) | ⚠️ NEW dominant blocker | ~2145 | `atomic_t<>` instantiated with a 12-byte struct (likely `u64+u32` packing differently on wasm32). Needs per-instantiation audit. |
| `cfg::_bool` / `cfg::_int<...>` constructor mismatches | ⚠️ remaining | ~3000+ | Cascade from earlier — investigate next |
| `cpu_thread` vs `ppu_thread` polymorphic conversion | ⚠️ remaining | ~1500 | Likely template instantiation cascade |
| `unknown type name '__m128i' / 'native_asm' / 'native_args'` | ⚠️ remaining | ~70 | x86 SSE + JIT helper types; need WASM SIMD substitutes |

After applying the 5 ✅ fixes above, error count went **7199 → 7065** — meaningful but the cascade is wider than the address_range hash alone. Each remaining root cause is a separate deep dive.

#### Session-end Phase 3 status

| Layer | Was | Now |
|---|---|---|
| `emcmake cmake` configure | broken | ✅ "Generating done" |
| Header-layer compile errors | 60 | ✅ 0 |
| Source-level compile errors | 0 → 7199 → 7065 | ⚠️ in progress |
| Source-level FAILED files | 0 → 448 → 448 | ⚠️ in progress |
| Link errors | n/a | n/a (haven't reached link yet) |
| `Emu.BootGame` returning | n/a | n/a |

**Realistic estimate for Phase 3 Option A completion: many weeks of focused work, probably 50-100 source-file touches.** Each remaining cluster (Align/cfg/cpu_thread/__m128i) is its own investigation thread.

**The architectural decision the project needs:**

- **Option A: wasm32 + invasive source audit.** Keep browser portability (Chrome/Firefox/Safari/iOS), but expect to touch 50-100 source files to make `usz`-based packing, x86 intrinsics, `utils::*` arch-helpers all wasm32-aware. Pthreads work → real PPU/SPU/RSX worker threads. Realistic effort: **months**.
- **Option B: wasm64 (`-sMEMORY64=1`).** 64-bit `size_t`, the `address_range.h` assert passes, fewer source touches. But: **no SharedArrayBuffer support** in `MEMORY64` mode currently → no pthreads → can't run PPU/SPU/RSX as workers. Effectively single-threaded WASM, which means single-threaded PS3 emulation. PS3 is fundamentally a multi-core platform (1 PPU + 6 SPUs); single-threaded interpretation would be very slow even *at the interpreter-only level*.
- **Option C: hybrid.** Build the wasm32 path; for the `usz` static_assert, provide a wasm32-specific code path that uses `uint64_t` explicitly for the hash packing. Less invasive than Option A but still needs the `utils::*` arch-helpers and `__m128i` substitutions.

None of these reach playable PS3 in this session, or even in the next several. The cheapest path to *any* real-emulation output: Option C + just enough WASM SIMD work to get PPU interpreter to compile, then attempt a hello-world `.self` boot (not retail game). Even that is multi-week.

#### After source-level: runtime blockers

Even when the emucore *links*, BootGame at runtime will hit:

- **mmap / sigsetjmp / signal-based page-fault handling** — used by `Emu/Memory/vm.cpp` and the PPU/SPU translators for guest-memory protection. Emcc doesn't fault on bad memory access the way native does; we need a different memory-protection strategy (probably software bounds-checking).
- **Real OS threading primitives** — `Emu/Cell/lv2/sys_*` syscalls assume sched_setaffinity, priority inheritance, NUMA awareness. None of these exist in Emscripten/web workers.
- **Inline assembly** — scattered through `util/atomic.hpp` and JIT paths. emcc's asm support is limited; needs C++ fallbacks.
- **named_thread<>** spawning — works under `-pthread` but workers consume pool slots; need to lazy-spawn to fit within iOS Safari's ~10-pthread cap.

#### Once configure clears, the source-level work

1. Strip `Emu/RSX/{GL,VK}` from the build; route through `CGSH_WebGPUJs` (Phase 5).
2. Force `PPU_DECODER_INTERPRETER` / `SPU_DECODER_INTERPRETER`. LLVM JIT doesn't work under emcc and needs Phase 6.
3. Stub `Emu/Cell/lv2/sys_*` syscalls that depend on real OS threading primitives (sched_setaffinity, priority inheritance, NUMA).
4. Replace signal-based page-fault handling (used by RSX/PPU memory protection) with WebAssembly-friendly equivalent. emcc doesn't fault on bad memory access the way native does.

#### 32-bit pointer hazard

This is the most worrying class of bug. rpcs3 uses `u64`-sized atomic operations on pointers in [util/atomic.hpp](rpcs3/util/atomic.hpp) and intrusive bit-stuffing in [util/shared_ptr.hpp](rpcs3/util/shared_ptr.hpp). On wasm32 these compile but silently do the wrong thing at runtime (upper 32 bits of "pointer" are garbage). Two options:

- **wasm32 + targeted fixes**: audit every `reinterpret_cast<u64>(ptr)` and similar. Many. Probably weeks.
- **wasm64 (`-sMEMORY64=1`)**: 64-bit pointers, no porting needed. But: experimental, Chrome-only, no SharedArrayBuffer support → no threads → no PPU/SPU workers. Trade-off: simpler porting, harder runtime.

#### Realistic effort

The plan's "XL · 2-3 weeks" estimate was optimistic — that's for someone who's done this kind of port before and has emcc + rpcs3 muscle memory. From cold, budget **6-12 weeks** of single-developer work to reach a successful `Emu.BootGame()` return, plus another 4-8 weeks before any pixels other than the existing Phase 1 textured-quad reach the canvas.

The Phase 1 "left pane renders via uploaded reference image" cheat is the best the POC has visually until Phase 5 lands.

---

## Phases

| # | Phase | Effort | Status |
|---|-------|--------|--------|
| 0 | Scaffold (React shell, embind surface, animated-clear render loop) | M | ✅ done |
| 1 | Textured-quad pipeline + reference-image presentation (left-pane "cheat" — JS uploads `terraria.png` so the compare view shows visual parity before real emulation exists) | M | ✅ done |
| 2 | `fs::file_base` JS-streamed disc subclass (`JsDiscBridge` + guarded `JsDiscFile` adapter; verified: 318 MB ISO round-trips, ISO9660 PVD `CD001` magic at 0x8001 matches byte-perfect) | M | ✅ done |
| 3 | Link emucore (interpreter only) into Rpcs3JS target — **DONE**: librpcs3_emu.a (56 MB), linked into Rpcs31024.wasm (9.3 MB); Emu.Init + Emu.BootGame both invocable; got past static_assert blocker via wasm32 patches (address_range hash, fat_ptr padding, native_asm stub, simd.hpp fallbacks, `-msimd128 -msse4.2`, JIT source exclusion) | XL | ✅ done |
| 4 | Wire `Emu.BootGame` into `Rpcs3VmJs::BootDiscImage`; smoke-boot `EBOOT.BIN` — **boots through the ELF loader, real firmware (liblv2.sprx) now executes, PPU runs ~1200 main_thread log entries deep**. See "Phase 4 deep-dive" below. Remaining: a deterministic game-internal null-fn triplet (LRs `0x7f9cc`/`0x7fa84`/`0x419168`) → `memory access out of bounds` on `main_thread`. | L | 🟡 deep — 1 crash chain left |
| 5 | RSX → WebGPU: replace `CGSH_WebGPUJs::Render` clear path with RSX command playback. WebGPUGSRender (rsx::thread subclass) is **wired and spawned**; renders an animated clear today. No runtime cellGcm calls reach it yet (blocked by Phase 4 crash). | XL | 🟡 scaffold wired |
| 6 | Cached AOT PPU/SPU (server-side LLVM → ship `.wasm` side modules) — required for >5 fps | XL | ⬜ |

Phase 1 is the **placeholder** that makes the compare view show parity *today*, before any emulation runs. Phases 3-5 replace the placeholder with real `Emu.BootGame` output; the Phase 1 reference-texture upload then becomes dead code (or stays as a fallback for diagnostic).

Phases 3-5 must land before the compare view renders real frames.
Phase 6 is what makes those frames playable instead of frame-per-second.

---

## Phase 4 deep-dive (current frontier)

`Emu.BootGame` now runs deep into PS3 boot. Progress this session, in
order of landing:

### 4a. Render backend swapped to WebGPU (was Null)
[`rpcs3/ui_js/Rpcs3VmJs.cpp`](rpcs3/ui_js/Rpcs3VmJs.cpp) `CreateVM`:
- `callbacks.init_gs_render` now spawns
  `g_fxo->init<rsx::thread, named_thread<WebGPUGSRender>>(ar)` instead of
  `NullGSRender`. [`WebGPUGSRender.{h,cpp}`](rpcs3/ui_js/WebGPUGSRender.h)
  is a real `rsx::thread` subclass; `end()` mirrors NullGSRender
  (`execute_nop_draw(); rsx::thread::end();`), `flip()` bumps an atomic
  `g_webgpu_flip_count`. **No flips yet** — once the crash was fixed
  (§4f) the game reaches its render loop and flips continuously at ~50 FPS.
- `callbacks.get_audio` returns `std::make_shared<NullAudioBackend>()`
  (was an empty `shared_ptr` → null-deref in `cell_audio_config::reset`).
- `callbacks.init_pad_handler` spawns `named_thread<pad_thread>`; the
  `pad_thread` ctor/dtor/`operator()` are stubbed in
  [`EmscriptenStubs.cpp`](rpcs3/ui_js/EmscriptenStubs.cpp) because the
  whole `Input/` tree doesn't cross-compile (SDL/hidapi/evdev).

### 4b. Potato config (minimise RAM + work per frame)
Set in `CreateVM` before boot: `llvm_precompilation=false`,
`max_spurs_threads=1`, `video.resolution=_480p`,
`frame_limit=_30`, `disable_vertex_cache=true`,
`audio.renderer=null`. (Note: `cfg::string` has no `.set()`; use
`.from_string()`.)

### 4c. Console noise silenced
The browser console was drowning in non-fatal spam that hid the real
trap. All fixed:
- `IsDebuggerPresent()` returns false early on `__EMSCRIPTEN__`
  ([`Utilities/Thread.cpp`](Utilities/Thread.cpp)) — was reading
  `/proc/self/status` = 527 lines.
- `thread_base::finalize()` skips the `getrusage` branch on emscripten.
- `printErr` filtered in [`Rpcs3Module.ts`](js/rpcs3_browser/src/Rpcs3Module.ts)
  and a global `console.warn` override in
  [`index.tsx`](js/rpcs3_browser/src/index.tsx) drop
  `warning: unsupported syscall: __syscall_*` (workers have per-worker
  printErr, so the Module-level filter alone is insufficient).

### 4d. wasm32 control-flow fixes (no `-fno-exceptions` traps)
BIND_FUNC's HLE return convention (`ppu.cia += 4`) and rpcs3's null/UNK
dispatch all assumed native exception unwinding. On wasm32 with
`-fno-exceptions` these became `unreachable` traps. Replaced with
PowerPC `blr`-emulation (`cia = lr & ~3`):
- `dump_regs` wasm32 stub at top of the function
  ([`PPUThread.cpp`](rpcs3/Emu/Cell/PPUThread.cpp)) — the disassembler /
  `std::any` / `vm::check_addr` path was the trap site at the top of
  every emergency-exit stack.
- `exec_task` null-fn dispatch: when `!fn->fn`, do
  `cia = u32(lr) & ~3u; continue;` (was `cpu_flag::stop`/`ret`, both of
  which stranded `main_thread`).
- UNK handler in [`PPUInterpreter.cpp`](rpcs3/Emu/Cell/PPUInterpreter.cpp):
  if `cia` is inside the `ppu_function_manager` HLE region, blr-emulate
  to `lr` instead of `throw_exception`.

### 4e. **Firmware loading via MEMFS preload** (the big one)
Without real firmware, every `cellXxx` call hit an HLE stub, and many
games immediately walk a firmware-provided function table → null. We now
load the real `.sprx` modules:

- **`public/dev_flash`** is a symlink to the developer's native rpcs3
  install (`~/Library/Application Support/rpcs3/dev_flash`, ~188 MB,
  copyrighted — **gitignored**, never committed). Each dev symlinks their
  own; firmware comes from their own `PS3UPDAT.PUP` install.
- **`preloadFirmwareFiles(mod)`** in
  [`Rpcs3Module.ts`](js/rpcs3_browser/src/Rpcs3Module.ts) fetches each
  path in `FIRMWARE_PRELOAD_PATHS` (liblv2, libsysmodule, libsysutil,
  libfs, libgcm_sys, libnet, librudp, libsre) and `FS.writeFile`s it
  into MEMFS. Runs between WASM init and DiscImageDevice init, wrapped in
  try/catch (non-fatal).
- **Path gotcha (resolved):** rpcs3 resolves `/dev_flash/` through its
  VFS mount to the host path
  `MEMFS_DEV_FLASH_HOST = /home/web_user/.config/rpcs3/dev_flash/`.
  Writing to the guest `/dev_flash/` path failed ("File not found");
  setting `g_cfg_vfs.dev_flash.from_string("/dev_flash/")` in C++ got
  clobbered by `Emu.Init`'s `from_default()+load()`. **Fix:** JS writes
  straight to the host MEMFS path.
- **Result: 33× boot progress** — `main_thread` log depth went 37 →
  1228 entries, all 8 SPRX load, 0 missing. Real kernel code runs:
  log lines now carry the `[liblv2: 0x00be4d60]` prefix where HLE stubs
  used to be.

### 4f. Blocker evolution: null-fn triplet → null-TOC cascade (FIXED) → ctors DO run; main() bails to RAII cleanup of an unconstructed .bss singleton (current)

**NOT actually fixed — only band-aided.** The null-fn dispatch no longer
kills the worker (`blr`-emulate lets `main_thread` limp on instead of
trapping `unreachable`), but the *underlying* fault is unresolved and the
game never reaches graphics init. Corrected root cause is in the "ACTUAL
root cause" box below; the original write-up (kept for history) follows:

The trap was BIND_FUNC's post-HLE-call `cia += 4` landing on the next OPD
slot (whose "raw" word is a function-descriptor pointer, not a PPC opcode).
The old `UNK`-in-HLE-region handler set `cpu_flag::ret` (stop the thread);
under `-fno-exceptions` the throw lowered to `unreachable` and killed the
worker. The fix in [`PPUInterpreter.cpp`](rpcs3/Emu/Cell/PPUInterpreter.cpp)
`UNK()` is proper PowerPC **`blr`-emulation**: set `ppu.cia = lr & ~3` and
return, so `cpu_task` resumes at the caller — exactly what a real `blr`
does. (Same band-aid already used in `PPUThread.cpp::exec_task`.) This edit
existed in the tree but the **emucore was never rebuilt**; the stage-1
rebuild this session compiled it for the first time and the crash vanished.

**Reality:** Terraria boots deep into init but STALLS before any render loop —
PPU analysis completes (5848 functions / 232232 blocks), `sys_memory_allocate`
succeeds, NetworkThread runs `cellNetCtlInit`, syscall stats show normal
`_sys_lwmutex_lock`/`_sys_lwcond_queue_wait`/`sys_timer_usleep` sync, and the
RSX thread **NEVER flips** (`g_webgpu_flip_count` stays 0) — `cellGcmInit`
is never called. main_thread band-aids past 3 null-fns then goes idle;
NetworkThread spins `cellNetCtlInit` forever. The orange on screen is
GSH_WebGPUJs's animated clear, not game output.**

> **ACTUAL root cause (CONFIRMED — supersedes the TOC=0 box below):**
> Two stages, both now nailed down:
>
> **Stage 1 — wrong TOC (FIXED, working).** `main_thread` was running EBOOT
> code with `r2` = liblv2's TOC `0xc08ab0` instead of the EBOOT TOC `0x5a7448`
> (cross-module calls into liblv2 clobber `r2`; the ELFv1 `ld r2,0x28(r1)`
> restore wasn't re-establishing it). Fix: a per-instruction guard in
> `PPUThread.cpp::exec_task` forces `r2 = eboot_toc` for every instruction in
> the EBOOT range `[0x10000,0x588ce8)` (PPC64 ELFv1: r2 is a per-module
> constant). `[TOC-ENTER]` fires on main_thread at `cia=0x103ac`; `[TOC-FIX]`
> re-fires inside the CRT init routines `0x49c1f8/0x49c2e8/0x4a0ba0` (they call
> liblv2 and come back with the wrong r2 — the guard self-heals each time).
> TOC-relative global loads now read correctly.
>
> **Stage 2 — G singleton's sub-object is never constructed (CURRENT BLOCKER).**
> `G` lives at EBOOT slot `0x59fcc8` (= `eboot_toc − 0x7780`) as a load-time
> relocated pointer `G = 0x9714f0`, a static singleton in seg-1 `.bss`
> (`[0x5b72c8, 0x9b9f38)`). The crash chain is now fully decoded:
> `main()` → type/RTTI lookup `0x40ea90` (compares arg against sibling
> registry `*0x5ace5c = 0x4cf6d8`, a VALID hash record) → lookup SUCCEEDS,
> dispatches handler → `0x1d6fb8` → dispatcher `0x7f8e4`:
> `lwz r6,-0x7780(r2)` (r6 = G = 0x9714f0) → `lwz r5,0(r6)` (**r5 = G[0] = 0**)
> → `lwz r28,0x10(r5)` (null deref) → vtable `blrl` to a null OPD at
> `lr=0x7f9cc` → `blr`-emulate band-aid → main goes idle.
>
> **`G[0]` is NEVER written by ANY thread the entire boot** — the
> all-thread/every-block `[G0-WRITE]` watch never fires. `[CTORW-LIST]`
> (stackwalk21) dumped the raw `.ctors` at `TOC[0x515c]=0x59006c`: it is
> **COMPLETE, not truncated** — GCC `-1` sentinel, exactly **6** ctor OPDs
> (`0x597d50/0x598818/0x599400/0x59a288/0x59d3b8/0x59eb98`; the old "26" was
> miscounting OPD descriptors), `0` terminator. `[CTORW] RETURN…@0x10474`
> confirms `G[0]=0` the instant the walk returns → **no ctor writes G[0]**.
> Hypothesis (a) "truncated ctor list" is DEAD.
>
> **ACTUAL shape (stackwalk23) — NOT a hard deadlock; the band-aid limps main
> PAST the crash, but it stalls before render init.** Decisive new evidence:
> there are only **3 null-fn calls total** (LR `0x7f9cc`, `0x7fa84`, `0x419168`),
> NOT an infinite loop — `blr`-emulate returns from each and the game keeps
> running (VM `IsRunning`→`booted=1`, GSH render loop reaches frame 721+).
> The real runtime boot sequence (from RPCS3's own HLE call logs) is:
> `sys_memory_allocate` ×N → prx load/start (libsysmodule, libsysutil_game,
> sysutil_np/np2) → `cellGameBootCheck` → `cellGameContentPermit` →
> `cellSysutilRegisterCallback(slot=0,func=*0x59af60)` → **[3 band-aided null
> `G[0]` vtable calls]** → `sceNp2Init`/`sceNpInit`/`sceNpManagerRegisterCallback`
> → `cellPadInit`/`cellKbInit` → `_sys_ppu_thread_create` ×N → **opens
> `/dev_bdvd/PS3_GAME/USRDIR/data.vfs`** (Terraria's real asset archive!) →
> spawns more threads → **STALLS. No `cellGcmInit`, no `cellGcmSetFlip`,
> `WebGPUGSRender::flip()` is NEVER called** (zero `[WebGPUGSRender] flip` logs).
> So nothing real is ever submitted to RSX → only the GSH fake clear shows.
>
> **Thread architecture:** `NetworkThread` (id `0x1000002`) and `WorkerThread`
> (id `0x1000003`) share the SAME entry `func=0x420fac, rtoc=0x5a7448` — a
> **generic job-worker pool**. Each idles in a 16 ms timed lwcond-wait loop at
> `0x3c4adc`(`sys_lwmutex_lock`)→`0x3c4af4`(`sys_lwcond_wait 16000µs`)→
> `0x3c4b00`(`sys_lwmutex_unlock`) waiting for **jobs that are never enqueued**.
> (`cellNetCtl` is HLE — `libnetctl.sprx` shows `Ignored module`; `libnet`/
> `librudp` are LLE.) This is normal idle-worker behavior, not the bug.
>
> **Refined root cause:** `G` (the global ptr at `0x9714f0`, set to a valid
> `.bss` addr at load time) should point to the game's **dispatch/sysutil
> manager**, but `G[0]=*(0x9714f0)` is **DEFINITIVELY never written** (the
> all-thread/every-block `[G0-WRITE]` watch never fires across the whole run).
> The 3 band-aided null calls ARE that manager's methods (likely the job-enqueue
> / init-dispatch path) — skipping them means the worker pool never gets the
> "load + init renderer" job, so the game never reaches `cellGcmInit`. **Fixing
> the render path is therefore gated on getting the game's OWN code to construct
> `G[0]`** (faking the object is forbidden and would just crash deeper on its
> real fields/vtable). stackwalk24 replaces the weak G-REF scan with a robust
> **setter-finder** (`[G-SET]` stw/std@0 with ≤64-instr backward G-load window,
> incl. callee-saved bases that survive `bl`) + **lazy-init detector**
> (`[G-CHK]` `lwz G; cmp-zero` getInstance pattern) to pinpoint the assignment
> site and why it's never reached. The failure is **G-specific, not systemic**
> (sibling registry `*0x5ace5c=0x4cf6d8` is a valid hash record; main's
> `[MAIN-TRACE]` is normal — `r6=0x2f6465765f626476`="/dev_bdv" disc setup).
> (`'abort'` in the log is a RED HERRING — RPCS3 *naming* libc `abort` at
> `0x4a0ecc`, not a call.)
>
> **stackwalk24/25 VERDICT — the lone G[0] writer is NOT the constructor.**
> The robust setter scan finds exactly ONE clean `-0x7780(r2)`-based writer of
> `G[0]`: `0x80170 stw r27,0(r31)` (r31=&G, loaded `0x8010c`), with **0
> null-check sites** (G[0] is not lazily guarded). But 0x80170 sits **inside
> the crash fn `0x7f8e4`** (prologue `0x7f8e4: f821fd51` = `stdu r1,-688`; no
> intervening `stdu/stwu r1` prologue down past 0x80178): that fn READS `G[0]`
> at `0x7f940` (null-deref `lwz r28,0x10(r5)` @`0x7f944`) and reads OLD
> `G[0]->0x10` at `0x80114` *before* the re-store at `0x80170` — so it ASSUMES
> `G[0]` already valid. It is a **use/reconfigure** fn, not the ctor. The
> `entry(mflr)=0x7f4c0` lead was a false positive (the backward scan matched
> only `mflr r0`=0x7c0802a6 and skipped the real `mflr r11`+`stdu` prologue).
> The real ctor must form `&G=0x9714f0` via the absolute `lis 0x97 / addi
> 0x14f0` pair (file-scope static-init) and store the allocated manager —
> stackwalk26 hunts that (`[G-CTOR]`). Net: G[0]'s constructor never runs and
> boot reaches the consumer (0x7f8e4) with G[0] still 0.
>
> **stackwalk26/27 BREAKTHROUGH — the ctor is a global ctor that ABORTS on a
> `sys_rwlock_create` EFAULT.** `[G-CTOR] 0` absolute formations + `nextProlog
> @0x80414 inside=yes` confirmed the eboot has no `lis/addi` &G writer and
> 0x80170 is the consumer's re-store. The real ctor is **global ctor body#1**
> (`[CTORW] body#1 entry=0x42fa28`, desc 0x59d3c0 ≈ OPD#6 0x59d3b8): the boot
> log shows it calling `sys_rwlock_create(rw_lock_id=*0x971664, attr=*0x0)` at
> `0x42f92c` → **`CELL_EFAULT`** (`sys_rwlock.cpp:38` rejects null `attr`).
> Geometry nails it: `0x971664 = 0x9714f0 + 0x174`, so **`G` is a static .bss
> struct** with the manager-ptr field at +0x0 (=G[0], what the crash reads)
> and an rwlock-id at +0x174. The ctor creates the rwlock, then *should*
> allocate+store the manager at +0x0 — but the EFAULT plausibly makes it bail
> first, leaving G[0]=0. PS3 SDK always passes a valid `attr`, so `attr=0` is
> likely a wasm32 arg-setup bug (r4 should be a stack ptr `r1+N`). stackwalk27
> adds `[RWL-ARGS]` (live r1/r3/r4 at 0x42f92c) + `rwl @` disasm of the
> ctor's arg-setup to decide bug-vs-design. **Fix path: make rwlock_create
> succeed → ctor finishes → G[0] set → boot advances toward cellGcmInit.**

> **stackwalk28 — rwlock fix LANDED & WORKS, but G[0] is still 0 (the rwlock
> was NOT the G[0] gate).** Relaxed `sys_rwlock_create` to accept `attr==NULL`
> with defaults (PRIORITY, not-process-shared) — `sys_rwlock.cpp`. Result: the
> EFAULT is gone (log: `null attr → default`), and boot goes **materially
> deeper**: `sys_memory_allocate` of 1MB/12MB/**84MB** (RSX-sized) now succeeds,
> `rsx::thread` spawns (`init_gs_render: rsx::thread initialized`), Network/Worker
> threads start. BUT `[CTORW] RETURN to _init: dispatched=25 returned=26
> G[0]=*(0x9714f0)=0` — **all 25 static ctors finish and G[0] is STILL 0**, so
> the global ctor was never the G[0] writer. The crash recurs: `null-fn DIAG`
> ×3 at lr=0x7f9cc/0x7fa84/0x419168. Full disasm of 0x7f8e4 decoded the cascade:
> `0x7f940 lwz r5,0(r6)` (r6=*(TOC-0x7780)=0x9714f0=G) reads **G[0]=0**, then
> `0x7f944 lwz r28,0x10(r5)` derefs **with no null-guard** → garbage chain →
> null `bclrl @0x7f9c8`. Worse: the crash `this` (`r25=0x9488c0`) ALSO has a
> null vtable (`*r25=[0,0]`) — **two unconstructed static managers**. Call path
> (stack-walk): `_start→CRT→main@0x142144 →bl 0x40ea90 (GUID/type-registry walk,
> head *(TOC+0x5a14)=0x4cf6d8) →bctrl@0x409bf8 →0x1d6fb8 →0x7f8e4`. The ONLY
> G[0] writer is the in-range store `@0x80170` (G-load@0x8010c) — but 0x7f8e4
> reads G[0] *without* a guard, so it can't also be the lazy constructor. ⇒ a
> **separate leaf getInstance/ctor** (no `stdu` prologue → boundary scan missed
> it) must live in `0x7f9d0..0x8010c` and own 0x80170. **stackwalk29** disasms
> that region + the `0x1d6fb8`/`0x409bf8` callers to find the `lwz G; cmpwi;
> beq construct` lazy-init and why the construct branch never runs before first
> use. Open question to settle next: is the 0x40ea90 registry path essential to
> reach `cellGcmInit`, or a skippable subsystem (Np/trophy/save) the band-aid
> can safely no-op? `[G0-WRITE]` has never fired ⇒ G[0] is genuinely never
> written ⇒ the construct path is simply never reached (ordering/branch bug).

#### Render reality (new frontier → task #11)
What's on screen is **NOT the game** yet: `GSH_WebGPUJs::Render` paints a
**fabricated sine-wave clear color** off its own `m_framesRendered` (orange
when booted, slate before) — a liveness indicator, not RSX output. And
`WebGPUGSRender::end()` is `execute_nop_draw()`, so no draw command is ever
executed; `flip()` only counts. To show real Terraria pixels we need one of:
1. **Framebuffer blit (cheap, IF data exists):** read the display buffer
   from guest VRAM at `rsx::get_address(display_buffers[info.buffer].offset,
   LOCAL)`, upload via `queue.WriteTexture`, sample as a fullscreen quad.
   Only works if the bytes are real pixels — a CPU/2D-engine-written surface.
   **A probe in `flip()` (this session) dumps center-row nonblack/distinct
   counts to decide.** If the GPU would've produced them, they'll be blank.
2. **Real RSX→WebGPU draw translator** (large): parse draw calls, vertex
   arrays, RSX fragment/vertex programs → WGSL, texture cache. Terraria is
   2D (textured quads) so a minimal subset may suffice, but still substantial.

(Historical decode of the old crash — `r6=*(TOC−0x7780)→r5=*r6→
r28=*(r5+0x10)=0→null call at 0x7f9c8`, zeroed heap object at `0x9488c0`
beyond EBOOT segs `0x10000–0x588c28`/`0x590000–0x5b72c8`, WorkerThread
`0x1000003` blocked — is preserved in git history; the blr-emulate fix made
it moot.)

**ROOT CAUSE — two-stage chain (confirmed this session):**

**Stage 1 (FIXED, verified working): null EBOOT TOC.** main_thread entered
the EBOOT body with `r2` (TOC) clobbered, so every `lwz rX,-off(r2)` global
load read garbage. Fix = per-instruction TOC repair in the dispatch loop:
when `cia` is in the EBOOT range and `gpr[2] != eboot_toc (0x5a7448)`, force
`gpr[2] = eboot_toc`. Confirmed firing: `[TOC-ENTER]` at cia=0x103ac (was
r2=0xc08ab0); `[TOC-FIX]` re-fires inside init routine 0x49c1a0. Kept.

**Stage 2 (CURRENT blocker): EBOOT C++ global constructors never run**, so
static singletons stay zero-filled. Decoded crash, function `0x7f8e4`
(prologue `stdu r1,-0x2b0(r1)`, saves r25–r31, `mr r25,r3` → r25 = `this`):

```
r6  = *(TOC − 0x7780) = G    ; G = game master singleton ptr (slot 0x59fcc8)
r5  = *r6  = G[0] = 0        ; ← vtable slot, NEVER written
r28 = *(r6 + 0x10) = 0       ; ← manager slot, NEVER written
... r7 derived = 0
r0  = *r7 = *(0) = 0         ; null read tolerated (mapped, zero-filled)
mtlr r0 ; blrl  →  cia = 0   ; ← null call → blr-emulate band-aid fires
```

Two distinct zeroed objects confirmed:
- **G itself** = `0x9714f0`, sitting in EBOOT seg1 .bss `[0x5b72c8,0x9b9f38)`.
  The pointer slot `0x59fcc8` holds G as a **load-time relocation** (already
  present at main_thread's first instr), but the *object* G points to is
  zero — a global ctor was supposed to fill `G[0]` (vtable) and `G[0x10]`
  (manager). The `[SLOT-WATCH vtable]`/`[SLOT-WATCH manager]` probes **never
  fire** across a full run → those writes never happen → G's ctor never ran.
- **r25 = `this` = `0x9488c0`** — also a zeroed object (`*r25 = [0,0]`).

**Call path (fixed back-chain stack walk):** `0x7f8e4` → ret `0x1d6fbc` →
`0x409bfc` → `0x142148` → `0x104d8` → `0x10250` (`_start`) → [entry, back=0].
~6 frames, a linear chain rooted at `_start`'s `bl 0x10354` (the CRT main,
likely `__libc_start_main`) — NOT a tight ctor-iterator loop.

**Real render-blocker = `WorkerThread` (`0x1000003`).** It spawns, reaches
"about-to-op", then executes ZERO PPU ops — blocked waiting on the very
singleton `main_thread` fails to initialise. Fixing ctor init unblocks RSX.

**RED HERRING (disproven this session):** the `[sys_prx] opt: ... lib_entries_ea
="_lv2", lib_stub_ea=0` garbage log is NOT the EBOOT loader. It comes from the
runtime syscall `_sys_prx_register_module` (`sys_prx.cpp:847`), where the *game*
passes the `opt` struct; for a non-VSH game with `type=0` it is a **no-op**
(returns `CELL_OK`/`ELF_IS_REGISTERED` without loading). The EBOOT's own
prx_param IS parsed correctly by `ppu_load_exec` (`PPUModule.cpp:2592`, which has
a hard `magic==0x1b434cec` check that would have aborted the boot otherwise), so
EBOOT imports/exports WERE resolved at load. The garbage is the game's own opt
buffer, never acted on.

**DISPROVEN — ctors DO run (cap_stackwalk9–12):** A runtime probe of the GCC
ctor walker `__do_global_ctors_aux` (`0x3bf8a8`, called by `_init` `0x10200`
at `0x10470`, BEFORE `main` at `0x104d4`) settled this empirically:
- The ctor list is **POPULATED**: `TOC[0x515c]=&__CTOR_END__=0x59006c`, sentinel
  `0xffffffff` @ `0x590000`, **26 entries** at `0x590004..0x590068` (descriptors
  `0x593df0..0x59ebf0`). So the loader DID load `.ctors` and the TOC slot is right.
- Each `.ctors` entry is a **pointer to an OPD** `{entry,toc}` (ELFv1); the walker
  loads `r0=*(entry)`, `r2=*(entry+4)`, `bctrl`. All 26 OPDs are **validly
  relocated** (e.g. `0x59ebf0→e=0x4916d8`, `0x593df0→e=0x10b08`, all `toc=0x5a7448`).
- A block-boundary trace confirms **all 26 ctors dispatch to real code** and run
  (`0x4916d8, 0x490bd4, 0x42fa28, … 0x10b08`). (Ctor #10 `0x1ac898` is heavy —
  ~28 M instrs in a tight loop at `0x33f24/0x33f68/0x1a57xx` — but it returns.)

So the C++ global ctors are NOT the blocker. (NB: this also means the earlier
"missing `.init_array` relocation" suspicion was wrong — `.ctors`/OPDs are fine.)

**CRT main decode confirms `0x141d84` = `main`.** `_start` (`0x10250`) calls
CRT main `0x10354` at `0x1024c`: it saves `argc/argv/envp`, runs two argv/envp
copy loops, then a pre-`main` init `bl`-sequence (`0x4cec48`, `0x4a0e64`,
`0x49c2d0`, `0x497cd8`, `0x49bcc0`, `0x4cec68`) — `_init`/`__do_global_ctors_aux`
lives in here and the 26 ctors run (above). Then at `0x104d4`:
`extsw r3,r28`(argc)/`rldicl r4,r27`(argv)/`rldicl r5,r29`(envp)/`bl 0x141d84`,
immediately followed by `bl 0x4920dc`(`exit`). That `main(argc,argv,envp);exit()`
shape proves **`0x141d84` = `main`**.

**CURRENT ROOT CAUSE — crash is inside `main()`'s init, on a null OBJECT
pointer (not a zero vtable).** The faulting `blrl` (DIAG #1, `lr=0x7f9cc`, site
`0x7f9c8`) decodes to `lwz r0,0(r7=r27); mtlr; blrl` and the stackwalk GPRs show
**`r27=0`** → it loads a function pointer through a NULL object and branches to
`cia=0`. The chain is `main(0x142144) → 0x40ea90(r3=r29,r4=r31) → 0x409a90 →
null blrl`. `main`'s body (`0x141d84`: `stdu r1,-0x310`, saves `r24..r31`) is a
long sequence of init `bl`s with conditional error checks (`bl 0x431674`@`0x141dcc`,
`bl 0x46b008`@`0x141df0`, … `cmpwi;beq`/`419d0584` patterns). `0x40ea90` is a
field-compare/lookup against the global at `*(TOC+0x5a14)` (first instr
`beq cr0,0x40ebc4`). So some init call in `main` returns 0/null/error, `main`
proceeds anyway, and a later lookup/RAII path dereferences the unconstructed
`.bss` singleton (`G=0x9714f0`, whose vtable `G[0]`/manager `G[0x10]` are never
written — SLOT-WATCH never fires).

**Next diagnostic (in flight):** the **MAIN-TRACE** probe in `PPUThread.cpp`
logs every block-entry `cia` within `[0x141d84..0x142150]` with `r3` (= each
`bl`'s return value) plus `r27/r29/r30/r31`. The first `bl`-return whose `r3` is
`0`/null/an error code pinpoints the failing init; the fix then targets that
HLE/lv2 call (or the singleton it was supposed to construct), not the loader —
`.ctors`/OPDs are confirmed fine, so the earlier "empty `.init_array`" suspicion
is **withdrawn**.

---

## Build & verify workflow

### Build

**⚠️ Two-stage build — easy to trip over.** There are two CMake/ninja
trees:
- `build_cmake/build/root-wasm/` builds the **emucore** archive
  `rpcs3/Emu/librpcs3_emu.a` (56 MB — PPU/SPU/RSX/lv2/vm, everything
  under `rpcs3/Emu/`).
- `build_cmake/build/wasm-ninja/` builds the **`Rpcs3` link target** and
  links the prebuilt `librpcs3_emu.a` from `root-wasm`.

`build_wasm.sh` only runs the **second** stage. So:
- Editing `rpcs3/ui_js/*` (Rpcs3VmJs, EmscriptenStubs, WebGPUGSRender,
  GSH_WebGPUJs, Main): `./build_wasm.sh <variant>` alone is enough.
- Editing anything under `rpcs3/Emu/` or `Utilities/` (PPUThread.cpp,
  PPUInterpreter.cpp, Thread.cpp, vm.cpp, …): you **must** rebuild the
  emucore first, or the link silently uses the stale archive:

```sh
source /Users/bank23232525/emsdk/emsdk_env.sh
# Stage 1 — only when an emucore source changed:
cmake --build build_cmake/build/root-wasm --target rpcs3_emu
# Stage 2 — always:
./build_wasm.sh 2048           # just the 2 GB variant (the compare URL default)
./build_wasm.sh                # all four variants
EMSDK=/Users/bank23232525/emsdk ./build_wasm.sh 1024   # without sourcing
```

(Verify a change actually landed:
`strings js/rpcs3_browser/public/Rpcs32048.wasm | grep '<new literal>'`.)

Output: `js/rpcs3_browser/{src,public}/Rpcs3{512,1024,2048,Dyna}.{js,wasm}`.

### Run

```sh
cd js/rpcs3_browser && npm start                       # PORT=3030
cd ~/roms && python3 -m http.server 8008               # ROM fileserver (separate shell)
```

Then:
- `http://localhost:3030/` — file picker
- `http://localhost:3030/?compare=terraria` — split-screen Live vs Ref
- `http://localhost:3030/?loadFixedRom=true&game=terraria` — autoboot

### Verify (headless screenshot — DO NOT use Claude Preview / MCP)

```sh
# Single-pane direct-boot view (autoloads ROM from localhost:8008)
node js/rpcs3_browser/capture_screenshot.js terraria live.png

# Same view with extra URL params
node js/rpcs3_browser/capture_screenshot.js terraria live.png "ram=2048&renderRes=854"

# Wait for a specific console marker before capturing (5th arg)
node js/rpcs3_browser/capture_screenshot.js terraria phase2.png "ram=1024" "JsDiscBridge: read"

# Side-by-side compare view (live left, reference right) — 6th arg = "compare"
node js/rpcs3_browser/capture_screenshot.js terraria compare.png "" "UploadReferenceTexture OK" "compare"
```

Writes to `/Users/bank23232525/.gemini/antigravity/scratch/<name>.png`.
Compare against `js/rpcs3_browser/public/terraria.png` — when the
left pane matches via real `Emu.BootGame` output (no longer the Phase 1
texture upload), Phase 5 is real.

The script watches the browser console for one of (5th arg overrides):
`JsDiscBridge: read`, `loaded ROM`, `BootDiscImage`, `initVmWebGPU completed`
— pulls the shot ~1.5 s after the first match, with a 60 s fallback
so it never hangs.

---

## Risks / open questions

1. **LLVM JIT is a hard blocker for playable framerates.** The interpreter
   path will boot but games will be unplayable. Phase 6 (server-side AOT
   that ships per-game `.wasm` side modules, or a wasm-emitting JIT) is
   not optional past "tech demo". Budget for it before promising
   playable retail titles.

2. **RSX → WGSL coverage is unbounded.** The shader translator is the
   single biggest chunk of work past Phase 4. Practical milestones:
   solid-color quad → textured quad (✅ done as scaffold) → XMB wave
   → first game title screen → general coverage.

3. **iOS WebGPU caveats.** Storage textures in fragment shaders were
   missing on Safari 26.0; if RSX programs need them, plan a degraded
   path. emdawnwebgpu is young — expect upstream bug reports.

4. **OPFS quota on iOS (~1 GB).** Firmware (~256 MB) fits, full retail
   BD-ROMs (25 GB) don't. POC scope: PSN-style smaller titles. Stretch:
   incremental install for run-from-disc games.

5. **PS3 disc decryption keys are user-supplied.** Same legal posture as
   native rpcs3 — the React shell prompts for `.IRD`, we don't ship any
   keys.

6. **Memory pressure on 1 GB fixed-heap WASM.** PS3 main RAM (256 MB) +
   RSX VRAM (256 MB) + JIT cache + Emscripten overhead + RSX command
   buffers is tight. 1024 is the target variant; 2048 has headroom but
   iOS Safari may refuse to allocate it. The 512 variant is for
   testing only.

---

## References — files to know

In this repo:

- [`rpcs3/rpcs3qt/main_window.cpp:751`](rpcs3/rpcs3qt/main_window.cpp#L751) — `BootISO()`, the seam we copy.
- [`rpcs3/rpcs3qt/main_window.cpp:541`](rpcs3/rpcs3qt/main_window.cpp#L541) — `Boot()`, the function `BootISO` calls.
- [`rpcs3/Emu/System.h`](rpcs3/Emu/System.h), [`System.cpp`](rpcs3/Emu/System.cpp) — `Emulator`, `BootGame`, `Init`.
- [`rpcs3/Loader/disc.cpp`](rpcs3/Loader/disc.cpp), [`Loader/ISO.cpp`](rpcs3/Loader/ISO.cpp) — ISO mount path.
- [`rpcs3/Utilities/File.h`](rpcs3/Utilities/File.h) — `fs::file_base`, the subclass target for streaming.
- [`rpcs3/ui_js/Rpcs3VmJs.cpp`](rpcs3/ui_js/Rpcs3VmJs.cpp) — printf stubs that need to call `Emu.*`.
- [`rpcs3/ui_js/Main.cpp`](rpcs3/ui_js/Main.cpp) — embind surface + WebGPU init + main-loop tick.
- [`rpcs3/ui_js/GSH_WebGPUJs.cpp`](rpcs3/ui_js/GSH_WebGPUJs.cpp) — `wgpu::Surface`, fullscreen-quad pipeline, render loop.
- [`rpcs3/Emu/RSX/GL/GLGSRender.cpp`](rpcs3/Emu/RSX/GL/GLGSRender.cpp), [`Emu/RSX/VK/VKGSRender.cpp`](rpcs3/Emu/RSX/VK/VKGSRender.cpp) — Phase 5 reference for what a "real" backend looks like.
- [`build_wasm.sh`](build_wasm.sh), [`js/rpcs3_browser/capture_screenshot.js`](js/rpcs3_browser/capture_screenshot.js) — build + verify.

In the React shell:

- [`js/rpcs3_browser/src/Rpcs3Module.ts`](js/rpcs3_browser/src/Rpcs3Module.ts) — WASM bootstrap, WebGPU device handoff, ref-image upload.
- [`js/rpcs3_browser/src/Actions.ts`](js/rpcs3_browser/src/Actions.ts) — Redux `init` + `bootFile` thunks.
- [`js/rpcs3_browser/src/DiscImageDevice.ts`](js/rpcs3_browser/src/DiscImageDevice.ts) — `File.slice` bridge for Phase 2.
- [`js/rpcs3_browser/src/Compare.tsx`](js/rpcs3_browser/src/Compare.tsx), [`Games.ts`](js/rpcs3_browser/src/Games.ts) — split-screen view + game registry.
