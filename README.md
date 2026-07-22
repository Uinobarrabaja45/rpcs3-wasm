RPCS3-WASM
==========

A fork of [**RPCS3**](https://github.com/RPCS3/rpcs3) — the open-source PlayStation 3 emulator — focused on compiling to **WebAssembly** so it runs directly in a web browser.

The emulator core is built with [Emscripten](https://emscripten.org/) and driven by a React front-end that renders through **WebGPU**. PPU/SPU JIT execution runs on pthread-backed WASM workers, which is why the app requires `SharedArrayBuffer` (and therefore COOP/COEP headers) at runtime.

## RTM EMU on the App Store

This browser build is packaged and shipped as part of **RTM EMU**:

[**RTM EMU — PS3, PS2, PC Games on the App Store**](https://apps.apple.com/us/app/rtm-emu-ps3-ps2-pc-games/id6756944097)

## Running

### Prerequisites

| Tool | Purpose |
| --- | --- |
| [Emscripten SDK (emsdk)](https://emscripten.org/docs/getting_started/downloads.html) | Compiles the C++ core to WASM |
| CMake + Ninja | Build system for the WASM target |
| Python 3 | Used by the build script (and the static server) |
| Node.js (18+ recommended) | Runs the React front-end |

### 1. Build the WASM core

The `.wasm` / `.js` artifacts are **not committed** to this repo (they're ~30 MB per variant), so you must build them before the front-end will run.

```bash
# Point the shell at your emsdk install
source /path/to/emsdk/emsdk_env.sh

# Build a memory variant — 1024 is the recommended default
./build_wasm.sh 1024
```

Recognised variants are `512`, `1024`, `2048`, and `Dyna`. Passing no arguments builds all four.

| Variant | Memory | Notes |
| --- | --- | --- |
| `512` | 0.5 GB | Size-optimised (`-Oz`) for low-RAM devices |
| `1024` | 1.0 GB | **Default** — comfortable floor for JIT + Emscripten overhead |
| `2048` | 2.0 GB | Headroom for commercial games |
| `Dyna` | growable | Starts small and grows; for memory-constrained devices |

> A PS3 baseline is 256 MB main RAM + 256 MB RSX VRAM. Once the JIT translation cache and Emscripten overhead are added, 1 GB is the practical minimum.

The script deploys the built `Rpcs3<variant>.js` and `Rpcs3<variant>.wasm` into `js/rpcs3_browser/src/` and `js/rpcs3_browser/public/`.

### 2. Run the front-end

```bash
cd js/rpcs3_browser
npm install
npm start
```

Then open:

```
http://localhost:3030/?ram=1024&webgpu=true
```

The `ram` parameter must match a variant you actually built. Other supported URL parameters include `game`, `hd`, `mute`, and `compare`.

The dev server sets the required `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` headers automatically (see `config-overrides.js`) — without them `SharedArrayBuffer` is unavailable and the emulator will not start.

### Production build

```bash
npm run build
```

The output **must** be served with the same COOP/COEP headers. A minimal static server that sets them is included:

```bash
python3 server.py 8000
```

## Building the native emulator

For the standard desktop builds of RPCS3, see [BUILDING.md](BUILDING.md).

## License

Most files are licensed under the terms of the GNU GPL-2.0-only License; see the [LICENSE](LICENSE) file for details. Some files may be licensed differently; check the appropriate file headers for details.

All credit for the underlying emulator goes to the [RPCS3 team](https://github.com/RPCS3/rpcs3) and its contributors.
