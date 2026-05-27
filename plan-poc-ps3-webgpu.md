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
`InputProviderEmscripten.cpp`, `GSH_WebGPUJs.cpp` — no emucore. Phase
shift: pull in `rpcs3/Emu/*` under a `EMSCRIPTEN` guard that strips the
deps it can't satisfy:

```cmake
if(EMSCRIPTEN)
    set(USE_VULKAN OFF CACHE BOOL "" FORCE)
    set(WITH_LLVM  OFF CACHE BOOL "" FORCE)          # → interpreter only
    set(USE_FAUDIO OFF CACHE BOOL "" FORCE)
    set(USE_DISCORD_RPC OFF CACHE BOOL "" FORCE)
    set(USE_LIBEVDEV    OFF CACHE BOOL "" FORCE)
    set(USE_HIDAPI      OFF CACHE BOOL "" FORCE)
    set(USE_LIBUSB      OFF CACHE BOOL "" FORCE)
    set(USE_OPENCV      OFF CACHE BOOL "" FORCE)
    add_subdirectory(rpcs3/Emu emucore)
endif()
target_link_libraries(Rpcs3 PRIVATE emucore)
```

This is a CMakeLists archaeology dig (2-3 weeks of "remove this dep,
fix the next 30 emcc errors, repeat"). Order of operations:

1. Strip `Emu/RSX/{GL,VK}` from the build; route everything through
   `CGSH_WebGPUJs` (which currently does nothing RSX-shaped — Phase 4
   below).
2. Force `PPU_DECODER_INTERPRETER` and `SPU_DECODER_INTERPRETER`. LLVM
   JIT doesn't work under emcc and never will without a wasm-emitting
   backend (see *Risks*).
3. Replace `Emu/Cell/lv2/sys_*` syscalls that need real OS threading
   primitives with no-ops or browser-friendly equivalents. Expect to
   stub more than you'd like at first.

---

## Phases

| # | Phase | Effort | Status |
|---|-------|--------|--------|
| 0 | Scaffold (React shell, embind surface, animated-clear render loop) | M | ✅ done |
| 1 | Textured-quad pipeline + reference-image presentation | M | ✅ done |
| 2 | `fs::file_base` JS-streamed disc subclass | M | ⬜ |
| 3 | Link emucore (interpreter only) into Rpcs3JS target | XL | ⬜ |
| 4 | Wire `Emu.BootGame` into `Rpcs3VmJs::BootDiscImage`; smoke-boot `EBOOT.BIN` | L | ⬜ |
| 5 | RSX → WebGPU: replace `CGSH_WebGPUJs::Render` clear path with RSX command playback | XL | ⬜ |
| 6 | Cached AOT PPU/SPU (server-side LLVM → ship `.wasm` side modules) — required for >5 fps | XL | ⬜ |

Phases 2–5 must land before the compare view can render real frames.
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
node js/rpcs3_browser/capture_screenshot.js terraria live.png
node js/rpcs3_browser/capture_screenshot.js terraria live.png "ram=2048&renderRes=854"
```

Writes to `/Users/bank23232525/.gemini/antigravity/scratch/live.png`.
Compare against `js/rpcs3_browser/public/terraria.png` — when they
match (or are close), Phase 5 is real.

The script watches the browser console for one of:
`loaded ROM`, `BootDiscImage`, `initVmWebGPU completed` — pulls the
shot ~1.5 s after the first match, with a 60 s fallback so it never
hangs.

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
