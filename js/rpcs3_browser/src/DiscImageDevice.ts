// Streams a PS3 ISO / .bin / .pkg from the user-selected File into the WASM
// heap. The C++ side (JsDiscBridge::ReadAt) calls read() via MAIN_THREAD_EM_ASM
// then polls isDone().
//
// IMPORTANT (perf/deadlock): the original async path —
//   file.slice(off,len).arrayBuffer().then(buf => HEAPU8.set(buf, dst))
// round-trips through the browser MAIN thread and completes on a *microtask*.
// As the WebGPU render loop ramps up it monopolises the main thread, starving
// that microtask, so the disc read's doneFlag never flips and the C++ ReadAt
// poll-loop spins forever — hanging EE ResourceLoop's data.vfs reads. That is
// the recurring producer-consumer "deadlock" (main waits on resources that
// never decompress because the producer is stuck mid-read).
//
// FIX: preload the entire image into a Uint8Array once (setFile), then serve
// reads SYNCHRONOUSLY (a single memcpy inside the MAIN_THREAD_EM_ASM call, no
// async, no polling). One quick main-thread slot per read — render-loop-proof.
export default class DiscImageDevice {
    module: any;
    doneFlag: Boolean;
    file: File | null;
    discData: Uint8Array | null;

    constructor(module: any) {
        this.module = module;
        this.doneFlag = false;
        this.file = null;
        this.discData = null;
    }

    read(dstPtr: number, offset: number, size: number) {
        if (!this.file) {
            throw new Error("No file set.");
        }
        this.doneFlag = false;
        // wasm passes the >2GiB heap pointer as a signed i32 (negative); coerce
        // to the real unsigned address before indexing HEAPU8.
        const dst = dstPtr >>> 0;

        // FAST PATH: whole disc resident in memory → synchronous copy, no async.
        if (this.discData) {
            try {
                const end = Math.min(offset + size, this.discData.length);
                if (end > offset) {
                    this.module.HEAPU8.set(this.discData.subarray(offset, end), dst);
                }
            } catch (e) {
                console.error('[DiscImageDevice] sync HEAPU8.set failed @0x' + dst.toString(16) +
                    ' off=' + offset + ' size=' + size + ' heapLen=' + (this.module.HEAPU8 ? this.module.HEAPU8.length : -1), e);
            }
            this.doneFlag = true;
            return;
        }

        // FALLBACK: disc not fully preloaded yet (early boot) → async slice read.
        const subsection = this.file.slice(offset, offset + size);
        subsection.arrayBuffer().then((value: ArrayBuffer) => {
            try {
                this.module.HEAPU8.set(new Uint8Array(value), dst);
            } catch (e) {
                console.error('[DiscImageDevice] async HEAPU8.set failed @0x' + dst.toString(16) + ' size=' + size, e);
            }
            this.doneFlag = true;
        }).catch((err: any) => {
            console.error('[DiscImageDevice] slice.arrayBuffer() rejected @0x' + offset.toString(16) + ' size=' + size, err);
            this.doneFlag = true;
        });
    }

    getFileSize() {
        if (!this.file) {
            throw new Error("No file set.");
        }
        return this.file.size;
    }

    isDone() {
        return this.doneFlag;
    }

    setFile(file: File) {
        this.file = file;
        this.discData = null;
        // Preload the whole image into memory so reads can be served synchronously
        // (see read()). ~300MB for a PS3 disc — fits alongside the wasm heap.
        file.arrayBuffer().then((buf: ArrayBuffer) => {
            this.discData = new Uint8Array(buf);
            console.log('[DiscImageDevice] disc preloaded into memory: ' + buf.byteLength +
                ' bytes — synchronous reads enabled (render-loop-proof)');
        }).catch((e: any) => {
            console.error('[DiscImageDevice] disc preload failed (staying on async path)', e);
        });
    }
}
