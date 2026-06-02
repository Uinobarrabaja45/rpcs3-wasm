// Browser-side wrapper around the Emscripten-built RPCS3 WASM module.
//
// Mirrors Play!.js's PlayModule.ts: pick a memory variant via ?ram=, locate
// the matching .js/.wasm pair, hand-off WebGPU device + adapter to the C++
// side via embind initVmWebGPU, set up DiscImageDevice for streaming the
// PS3 ISO. The PS3's address space is wider than the PS2's (32-bit user
// space plus separate RSX VRAM), so the default heap is larger.
//
// Variants:
//   ?ram=512  → Rpcs3512  (small profile, 512 MB heap — testing only)
//   ?ram=1024 → Rpcs31024 (default — fits typical commercial games)
//   ?ram=2048 → Rpcs32048 (larger games + state with headroom)
//   ?ram=dyna → Rpcs3Dyna (growable memory; slower per-alloc, smaller start)
import Rpcs3512 from "./Rpcs3512";
import Rpcs31024 from "./Rpcs31024";
import Rpcs32048 from "./Rpcs32048";
import Rpcs3Dyna from "./Rpcs3Dyna";
import DiscImageDevice from "./DiscImageDevice";

export let Rpcs3Module: any = null;

// `ignore_log` flag — default TRUE. When set, the WASM module's stdout
// (every printf from the emulator core / log channels) is routed to a no-op
// so the page console stays quiet. `printErr` is left intact so genuine
// WASM aborts still surface. index.tsx silences the JS-side console too.
const ignoreLog = new URLSearchParams(window.location.search).get('ignore_log') !== 'false';

// Emscripten's libc prints "warning: unsupported syscall: __syscall_X" via
// `err()` (which defaults to console.warn) every time rpcs3 calls mlock /
// mprotect / madvise / getrusage / etc. The stubs return success so these
// are pure noise — but they fire hundreds of times per boot and bury real
// errors. Filter them at the printErr boundary; keep everything else.
const SYSCALL_NOISE_RE = /^warning: unsupported syscall: __syscall_/;
const printErrFiltered = (text: any) => {
    if (typeof text === 'string' && SYSCALL_NOISE_RE.test(text)) return;
    // Fall back to console.warn for genuine wasm aborts / unhandled errors.
    // eslint-disable-next-line no-console
    console.warn(text);
};

let module_overrides: { [k: string]: any } = {
    locateFile: function (path: string) {
        const baseURL = window.location.origin + window.location.pathname.substring(0, window.location.pathname.lastIndexOf("/"));
        return baseURL + '/' + path;
    },
    mainScriptUrlOrBlob: "",
    printErr: printErrFiltered,
};
if (ignoreLog) {
    module_overrides.print = () => {};
}

function getRAMFromURL(): string | null {
    return new URLSearchParams(window.location.search).get('ram');
}

// PS3 firmware .sprx files we preload into Emscripten MEMFS before boot.
// These are the modules whose absence forces the rpcs3 loader to fall
// back to HLE — which works for early init but leaves dispatch tables
// (cellSysutil callback chain, sceNp export thunks, sysPrxForUser
// callback registration) only partially populated. Games like Terraria
// dereference those tables and segfault when entries are NULL.
//
// Minimal set chosen to fix the cellSysutilRegisterCallback null-fn
// dispatch path observed in main_thread:
//   - liblv2.sprx        — kernel runtime, callback dispatch
//   - libsysmodule.sprx  — runtime module loader (cellSysmoduleLoadModule)
//   - libsysutil.sprx    — sysutil callback infrastructure
// More can be added as the boot progresses past each new null-fn site.
const FIRMWARE_PRELOAD_PATHS = [
    // Boot/kernel/runtime
    '/dev_flash/sys/external/liblv2.sprx',
    '/dev_flash/sys/external/libsysmodule.sprx',
    '/dev_flash/sys/external/libsysutil.sprx',
    // Modules Terraria asks for at boot (observed via
    // "firmware SPRX not found" log lines). Adding them lets the
    // loader resolve real exports instead of HLE-stubbing them.
    '/dev_flash/sys/external/libfs.sprx',
    '/dev_flash/sys/external/libgcm_sys.sprx',
    '/dev_flash/sys/external/libnet.sprx',
    '/dev_flash/sys/external/librudp.sprx',
    '/dev_flash/sys/external/libsre.sprx',
    // Audio codec firmware images. Terraria's "Multistream" audio engine
    // opens flashMP3.pic at boot (sys_fs_open) and calls abort() —
    // "mp3 failed to load MP3 codec (are you using the correct flash?)" —
    // if it's absent. The real firmware files live in public/dev_flash but
    // were never preloaded into MEMFS. flashATRAC.pic added alongside since
    // the same engine init path loads the ATRAC codec too.
    '/dev_flash/sys/external/flashMP3.pic',
    '/dev_flash/sys/external/flashATRAC.pic',
];

// rpcs3 mounts /dev_flash → `$(EmulatorDir)dev_flash/` which on
// Emscripten resolves to /home/web_user/.config/rpcs3/dev_flash/. We
// write firmware .sprx to that host path, NOT to the guest /dev_flash/
// path, because the loader resolves through the mount before touching
// the FS. (Tried overriding g_cfg_vfs.dev_flash in CreateVM — gets
// clobbered by Emu.Init()'s from_default + load chain.)
const MEMFS_DEV_FLASH_HOST = '/home/web_user/.config/rpcs3/dev_flash/';

async function preloadFirmwareFiles(mod: any): Promise<void> {
    console.log('[firmware] preloading .sprx files into MEMFS...');
    const base = `${process.env.PUBLIC_URL || ''}`;
    let totalBytes = 0;
    let loadedCount = 0;
    let missingCount = 0;

    for (const guestPath of FIRMWARE_PRELOAD_PATHS) {
        // guestPath like "/dev_flash/sys/external/liblv2.sprx".
        // Fetch URL is server-side (under public/dev_flash/...) — same
        // layout, easy. WRITE path is the host-side mount target.
        const fetchUrl = base + guestPath;
        const relative = guestPath.replace(/^\/dev_flash\//, '');
        const memfsPath = MEMFS_DEV_FLASH_HOST + relative;
        try {
            const resp = await fetch(fetchUrl);
            if (!resp.ok) {
                console.warn(`[firmware] missing ${guestPath} (HTTP ${resp.status}) — falling back to HLE`);
                missingCount++;
                continue;
            }
            const buf = new Uint8Array(await resp.arrayBuffer());
            // Create parent dirs in MEMFS for the host-side path.
            const parts = memfsPath.split('/').filter(Boolean);
            let cur = '';
            for (let i = 0; i < parts.length - 1; i++) {
                cur += '/' + parts[i];
                try { mod.FS.mkdir(cur); } catch (_) { /* EEXIST */ }
            }
            mod.FS.writeFile(memfsPath, buf);
            totalBytes += buf.length;
            loadedCount++;
            console.log(`[firmware] loaded ${guestPath} → ${memfsPath} (${buf.length} bytes)`);
        } catch (e) {
            console.warn(`[firmware] fetch failed for ${guestPath}:`, e);
            missingCount++;
        }
    }

    console.log(`[firmware] preload complete: ${loadedCount} files (${(totalBytes/1024).toFixed(0)} KB), ${missingCount} missing`);
}

async function loadRpcs3Module(ramParam: string | null): Promise<{ moduleFunction: any; jsFileName: string }> {
    let moduleFunction: any;
    let jsFileName: string;

    console.log('Loading Rpcs3 module with RAM setting:', ramParam);

    // Default = 2048 MB (Rpcs32048). The wasm32 vm layer pre-reserves a
    // 1.5 GiB host-side g_base_addr region at static-init (aligned_alloc
    // inside __wasm_call_ctors before main() runs) to back the PS3 guest
    // address space. The 1024 MB build's INITIAL_MEMORY=1 GiB cannot grow
    // far enough to satisfy that allocation → aborts immediately with
    // "Cannot enlarge memory arrays to size 1672445952 bytes (OOM)".
    // Users can opt into smaller builds explicitly via ?ram=1024/512/dyna
    // once we shrink g_base_addr (Phase 5+ work), but 2048 is the safe
    // default that actually boots.
    if (ramParam === '512') {
        jsFileName = 'Rpcs3512.js';
        moduleFunction = Rpcs3512;
    } else if (ramParam === '1024') {
        console.warn('[Rpcs3Module] ?ram=1024 will OOM at static-init on current wasm32 build (g_base_addr=1.5GiB). Use ?ram=2048 unless you know what you are doing.');
        jsFileName = 'Rpcs31024.js';
        moduleFunction = Rpcs31024;
    } else if (ramParam === '2048') {
        jsFileName = 'Rpcs32048.js';
        moduleFunction = Rpcs32048;
    } else if (ramParam === 'dyna' || ramParam === 'Dyna') {
        jsFileName = 'Rpcs3Dyna.js';
        moduleFunction = Rpcs3Dyna;
    } else {
        // No ?ram=: pick 2048 (safe default — see comment above).
        jsFileName = 'Rpcs32048.js';
        moduleFunction = Rpcs32048;
    }
    return { moduleFunction, jsFileName };
}

// ?mute=true — wrap AudioContext so all output is fed through a zero-gain
// node. Useful for thermal/perf testing; cellAudio still runs in the WASM,
// only the final mix is muted at the host layer.
function applyMuteIfRequested() {
    const mute = new URLSearchParams(window.location.search).get('mute') === 'true';
    if (!mute) return;
    const OrigAC: typeof AudioContext = (window.AudioContext || (window as any).webkitAudioContext);
    if (!OrigAC) return;
    (window as any).__muteGainNodes = [];
    const Patched = function (this: any, ...args: any[]) {
        const ctx: AudioContext = new OrigAC(...(args as []));
        const gain = ctx.createGain();
        gain.gain.value = 0;
        gain.connect((ctx as any)._realDestination || ctx.destination);
        Object.defineProperty(ctx, 'destination', {
            get: () => gain,
            configurable: true,
        });
        (window as any).__muteGainNodes.push(gain);
        return ctx;
    } as any;
    Object.setPrototypeOf(Patched, OrigAC);
    Patched.prototype = OrigAC.prototype;
    window.AudioContext = Patched as typeof AudioContext;
    if ((window as any).webkitAudioContext) (window as any).webkitAudioContext = Patched;
}

applyMuteIfRequested();

export let initRpcs3Module = async function () {
    console.log('SharedArrayBuffer available:', typeof SharedArrayBuffer !== 'undefined');

    const ramParam = getRAMFromURL();
    const { moduleFunction, jsFileName } = await loadRpcs3Module(ramParam);

    console.log(`Loading Rpcs3 WASM module: ramParam=${ramParam || 'default'}, js=${jsFileName}`);

    module_overrides.mainScriptUrlOrBlob = module_overrides.locateFile(jsFileName);

    try {
        console.log('Loading Rpcs3 WASM module...');
        Rpcs3Module = await moduleFunction(module_overrides);
        console.log('Rpcs3 WASM module loaded successfully');
        try { (window as any).__rpcs3Module = Rpcs3Module; } catch (e) {}

        if (!Rpcs3Module) {
            throw new Error('Rpcs3Module is null or undefined');
        }
        if (!Rpcs3Module.HEAP8) {
            throw new Error('WASM module memory (HEAP8) not properly initialized');
        }
        console.log('WASM memory initialized, HEAP8 size:', Rpcs3Module.HEAP8.length);

        console.log('Creating /work directory in MEMFS...');
        Rpcs3Module.FS.mkdir("/work");

        // Pre-populate /dev_flash/ in MEMFS with critical PS3 firmware
        // .sprx files. rpcs3 only logs "Failed to load .../liblv2.sprx"
        // and falls back to HLE stubs when these aren't present — but the
        // HLE stubs leave gaps (null callback dispatch tables, missing
        // export resolution) that fail real games like Terraria. With
        // liblv2.sprx + libsysmodule.sprx in place, the loader links
        // against the real firmware exports.
        //
        // Files are served by the dev server from public/dev_flash/ (a
        // symlink to the host's existing rpcs3 install). Fetched only on
        // demand — we don't bundle 188 MB of firmware into the wasm.
        try {
            await preloadFirmwareFiles(Rpcs3Module);
        } catch (e) {
            console.warn('[firmware] preload failed (non-fatal — boot continues with HLE):', e);
        }

        console.log('Initializing DiscImageDevice...');
        Rpcs3Module.discImageDevice = new DiscImageDevice(Rpcs3Module);

        if (!Rpcs3Module.ready) {
            await Rpcs3Module;
        }

        // Wait for pthread workers (PPU / SPU JIT threads, RSX command
        // ring thread) to finish loading before initVm. Same iOS guard as
        // Play!.js — slower devices need a longer initial settle window.
        console.log('Waiting for worker threads to initialize...');
        const isIOS = /iPad|iPhone|iPod/.test(navigator.userAgent);
        const isLowRAMDevice = isIOS || (navigator.hardwareConcurrency && navigator.hardwareConcurrency <= 4);
        const delayMs = isLowRAMDevice ? 1500 : 500;
        await new Promise(resolve => setTimeout(resolve, delayMs));

        if (Rpcs3Module.PThread && Rpcs3Module.PThread.unusedWorkers) {
            const maxWaitTime = 3000;
            const startTime = Date.now();
            while (Date.now() - startTime < maxWaitTime) {
                const allWorkersLoaded = Rpcs3Module.PThread.unusedWorkers.every((worker: any) => worker.loaded);
                if (allWorkersLoaded) {
                    console.log('All worker threads loaded');
                    break;
                }
                await new Promise(resolve => setTimeout(resolve, 100));
            }
        }

        // PS3 native output is 1280x720 (16:9). Without ?hd=false we boot at
        // native; ?hd=false drops to 640x360 for thermal-bounded devices.
        const isHD = new URLSearchParams(window.location.search).get('hd') !== 'false';
        const defaultWidth = isHD ? 1280 : 640;
        const defaultHeight = isHD ? 720 : 360;
        const isPortrait = window.innerHeight > window.innerWidth;

        let canvas = document.getElementById('outputCanvas') as HTMLCanvasElement;
        if (!canvas) {
            console.warn('Canvas #outputCanvas not found, creating it...');
            canvas = document.createElement('canvas');
            canvas.id = 'outputCanvas';
            canvas.width = defaultWidth;
            canvas.height = defaultHeight;
            canvas.style.position = 'fixed';
            canvas.style.top = '0';
            if (isPortrait) {
                canvas.style.left = '50%';
                canvas.style.transform = 'translateX(-50%)';
            } else {
                canvas.style.left = '0';
                canvas.style.transform = '';
            }
            canvas.style.zIndex = isPortrait ? '1' : '-1';
            canvas.style.pointerEvents = 'none';
            document.body.appendChild(canvas);
            console.log(`Canvas created: ${canvas.width}x${canvas.height}, portrait=${isPortrait}`);
        } else if (canvas.width === 0 || canvas.height === 0 || isHD) {
            canvas.width = defaultWidth;
            canvas.height = defaultHeight;
        }

        const canvasBySelector = document.querySelector('#outputCanvas');
        if (!canvasBySelector) {
            throw new Error('Canvas #outputCanvas not found by CSS selector.');
        }
        console.log('Canvas #outputCanvas verified, accessible by CSS selector');

        // --- WebGPU initialization (the headline feature for this POC) ---
        //
        // For the PS3, the RSX (Reality Synthesizer) GPU is a heavily-
        // modified NV47. We back it with a WebGPU translator that maps RSX
        // FP/VP programs and command-buffer ops to WGSL render passes. The
        // browser hands us a GPUDevice + GPUAdapter; the WASM imports them
        // via the emdawnwebgpu --use-port plumbing and they become the
        // backing handles for our Dawn-compatible C++ surface.
        //
        // WebGL fallback is intentionally NOT provided here — RPCS3's
        // shaders (especially RSX fragment programs with their NV47-style
        // unbounded loops + texture reads-modify-writes) cannot be lowered
        // to WebGL2 in a useful way. WebGPU's storage buffers + compute
        // shaders are mandatory.
        const webgpuEnabled = new URLSearchParams(window.location.search).get('webgpu') !== 'false';
        let gpuDevice: any = null;
        let gpuAdapter: any = null;
        let useWebGPU = false;

        let webgpuRecoveryCount = 0;
        const WEBGPU_MAX_RECOVERIES = 8;

        const wireWebGPUDevice = (device: any) => {
            try {
                device.addEventListener('uncapturederror', (ev: any) => {
                    console.error('[WebGPU uncaptured error]', ev?.error?.message || ev);
                });
            } catch (e) {
                console.warn('[WebGPU] Could not install uncapturederror listener:', e);
            }
            try {
                device.lost.then((info: any) => {
                    console.error('[WebGPU device LOST]', info?.reason, info?.message);
                    recoverWebGPUDevice();
                });
            } catch (e) { /* ignore */ }
        };

        const recoverWebGPUDevice = async () => {
            if (webgpuRecoveryCount >= WEBGPU_MAX_RECOVERIES) {
                console.error(`[WebGPU] device lost ${webgpuRecoveryCount} times — giving up recovery`);
                return;
            }
            webgpuRecoveryCount++;
            console.warn(`[WebGPU] recovering lost device, attempt ${webgpuRecoveryCount}`);
            try {
                const adapter = await (navigator as any).gpu.requestAdapter();
                if (!adapter) {
                    console.error('[WebGPU] recovery failed — no adapter available');
                    return;
                }
                const device = await adapter.requestDevice();
                wireWebGPUDevice(device);
                gpuAdapter = adapter;
                gpuDevice = device;
                if (typeof Rpcs3Module.recreateWebGPUDevice === 'function') {
                    Rpcs3Module.recreateWebGPUDevice(device, adapter);
                    console.log('[WebGPU] new device handed to native recovery');
                } else {
                    console.error('[WebGPU] recreateWebGPUDevice export missing — cannot recover');
                }
            } catch (e) {
                console.error('[WebGPU] recovery failed:', e);
            }
        };

        if (webgpuEnabled && (navigator as any).gpu) {
            console.log('WebGPU detected, requesting adapter + device...');
            try {
                // Request a high-performance adapter — PS3 emulation is
                // notoriously GPU-bound, so we prefer dGPU on hybrid systems.
                gpuAdapter = await (navigator as any).gpu.requestAdapter({
                    powerPreference: 'high-performance',
                });
                if (gpuAdapter) {
                    gpuDevice = await gpuAdapter.requestDevice({
                        // RPCS3 leans on these for the RSX→WGSL translator.
                        // If a feature is unsupported the device falls back
                        // to defaults rather than failing outright.
                        requiredFeatures: [],
                        requiredLimits: {},
                    });
                    useWebGPU = true;
                    console.log('WebGPU device acquired successfully');
                    wireWebGPUDevice(gpuDevice);
                } else {
                    console.warn('WebGPU adapter not found');
                }
            } catch (e) {
                console.warn('Failed to initialize WebGPU:', e);
            }
        }

        let initResult;
        if (useWebGPU && typeof Rpcs3Module.initVmWebGPU === 'function') {
            try {
                console.log('Calling initVmWebGPU...');
                const canvasEl = document.getElementById('outputCanvas');
                if (!canvasEl) {
                    throw new Error('outputCanvas element not found in DOM — required for WebGPU surface creation');
                }
                initResult = Rpcs3Module.initVmWebGPU(gpuDevice, gpuAdapter, canvasEl, '');
                console.log('initVmWebGPU completed successfully');
            } catch (error) {
                console.error('initVmWebGPU failed:', error);
                useWebGPU = false;
                throw error;
            }
        } else if (typeof Rpcs3Module.initVm === 'function') {
            // Headless / no-WebGPU path. Won't render anything visible, but
            // still useful for testing the PPU/SPU JIT pipeline.
            try {
                initResult = Rpcs3Module.initVm();
                console.log('initVm (headless) completed');
            } catch (error: any) {
                console.error('initVm failed:', error);
                throw error;
            }
        } else {
            throw new Error('Neither initVmWebGPU nor initVm exported — WASM build is incomplete');
        }

        // Mark VM ready so the UI auto-load can proceed.
        Rpcs3Module.isVmInitialized = true;

        // ?renderRes=N — override final output canvas width. 720p (1280) is
        // the PS3 internal native render target for most titles; smaller
        // values trade sharpness for sustained-fps headroom.
        const renderResParam = new URLSearchParams(window.location.search).get('renderRes');
        if (renderResParam !== null && typeof Rpcs3Module.setResolution === 'function') {
            const w = parseInt(renderResParam, 10);
            if (!isNaN(w) && w >= 320 && w <= 1920) {
                Rpcs3Module.setResolution(w, Math.round(w * 9 / 16));
                console.log(`renderRes: output canvas set to ${w}x${Math.round(w * 9 / 16)} (16:9)`);
            }
        }
    } catch (error) {
        console.error('Error initializing Rpcs3Module:', error);
        if (error instanceof Error) {
            console.error('Error message:', error.message);
            console.error('Error stack:', error.stack);
        }
        if (Rpcs3Module) {
            console.error('Rpcs3Module state:', {
                hasHEAP8: !!Rpcs3Module.HEAP8,
                hasHEAP32: !!Rpcs3Module.HEAP32,
                hasFS: !!Rpcs3Module.FS,
                hasccall: !!Rpcs3Module.ccall
            });
        }
        throw error;
    }
};
