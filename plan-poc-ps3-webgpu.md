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
  `EnsureQuadPipeline()` + `UploadReferenceTexture()` +
  `DrawReferenceQuad()`. Today this is used to display the reference
  PNG so the compare view has something to look at; once `Emu.BootGame`
  is wired, the same pipeline can present the RSX framebuffer.
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

| Root cause | Cascading error count | Notes |
|---|---|---|
| `static_assert(sizeof(usz) >= 2 * sizeof(u32))` at [Utilities/address_range.h:586](Utilities/address_range.h#L586) | ~1400 | wasm32 has 4-byte `size_t`. Hash key packs two `u32`s into one `usz` — code is wrong on wasm32. Either: (a) switch to wasm64, (b) refactor to use `u64` explicitly. |
| "Missing `utils::bless()` / `get_tsc()` / `trap()` / `pause()` implementation" | ~720 | Arch-specific utils in `rpcs3/util/asm.hpp` — need wasm equivalents. `pause()`/`trap()` map to wasm intrinsics or noop; `get_tsc()` to `emscripten_performance_now()` × scale; `bless()` is type-pun, portable. |
| `unknown type name '__m128i'` (x86 SSE intrinsics) | ~7 + many | rpcs3 has ARM64 fallbacks for some of these; need a parallel WASM SIMD path via `wasm_simd128.h`. |
| `unknown type name 'native_asm' / 'native_args'` | ~30 | x86/arm64 specific JIT helper types. Stub for wasm. |
| Cascading `cfg::*` / `cpu_thread` / `ppu_thread` errors | ~3000+ | Knock-on of the above. Likely resolved once roots are fixed. |

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
| 3 | Link emucore (interpreter only) into Rpcs3JS target — **revised XL → 6-12 weeks**; configure now PASSES; missing-header errors went from 55 → 0 via stub headers in `3rdparty/_emscripten_stubs/include/`; build now exposes the **real architectural blocker**: `Utilities/address_range.h:586` `static_assert(sizeof(usz) >= 2 * sizeof(u32))` fails on wasm32 (4-byte `size_t`) → thousands of cascading errors | XL | 🟡 in progress |
| 4 | Wire `Emu.BootGame` into `Rpcs3VmJs::BootDiscImage`; smoke-boot `EBOOT.BIN` | L | ⬜ |
| 5 | RSX → WebGPU: replace `CGSH_WebGPUJs::Render` clear path with RSX command playback | XL | ⬜ |
| 6 | Cached AOT PPU/SPU (server-side LLVM → ship `.wasm` side modules) — required for >5 fps | XL | ⬜ |

Phase 1 is the **placeholder** that makes the compare view show parity *today*, before any emulation runs. Phases 3-5 replace the placeholder with real `Emu.BootGame` output; the Phase 1 reference-texture upload then becomes dead code (or stays as a fallback for diagnostic).

Phases 3-5 must land before the compare view renders real frames.
Phase 6 is what makes those frames playable instead of frame-per-second.

---

## Build & verify workflow

### Build

```sh
# Source emsdk first (or set EMSDK + EMSDK_NODE inline like the example below)
source /Users/bank23232525/emsdk/emsdk_env.sh

./build_wasm.sh                # all four variants
./build_wasm.sh 1024           # just the default 1 GB variant
EMSDK=/Users/bank23232525/emsdk ./build_wasm.sh 1024   # without sourcing
```

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
