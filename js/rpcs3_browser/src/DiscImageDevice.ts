// Streams a PS3 ISO / .bin / .pkg from the user-selected File into the WASM
// heap on demand. The C++ side calls back via embind to ask for chunks at a
// given byte offset; doneFlag is polled to know when the async slice() has
// resolved. Mirrors the Play! disc image bridge but without PS2-specific
// assumptions.
export default class DiscImageDevice {
    module: any;
    doneFlag: Boolean;
    file: File | null;

    constructor(module: any) {
        this.module = module;
        this.doneFlag = false;
        this.file = null;
    }

    read(dstPtr: number, offset: number, size: number) {
        if (!this.file) {
            throw new Error("No file set.");
        }
        this.doneFlag = false;
        const subsection = this.file.slice(offset, offset + size);
        subsection.arrayBuffer().then((value: ArrayBuffer) => {
            this.module.HEAPU8.set(new Uint8Array(value), dstPtr);
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
    }
}
