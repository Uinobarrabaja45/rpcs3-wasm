
import Ps3zReader, { Ps3zDirectoryEntry } from './Ps3zReader';

export interface Ps3zPathInfo {
    exists: boolean;
    isDirectory: boolean;
    size: number;
}

/**
 * Browser-side interface for exposing a PS3Z bundle to the WASM layer.
 *
 * This class does not boot the game by itself. It provides metadata,
 * directory listings and asynchronous random-access reads that the
 * C++ virtual filesystem bridge will call later.
 */
export default class Ps3zDevice {
    private readonly module: any;
    private readonly reader: Ps3zReader;

    private directoryCache = new Map<string, Ps3zDirectoryEntry[]>();

    doneFlag = true;
    lastError = '';

    constructor(module: any, reader: Ps3zReader) {
        if (!module || !module.HEAPU8) {
            throw new Error('[PS3Z] El módulo WASM no está preparado.');
        }

        this.module = module;
        this.reader = reader;
    }

    hasBundle(): boolean {
        return !!this.reader;
    }

    /**
     * Converts a virtual-device relative path into a safe PS3Z path.
     * Empty path and "." represent the bundle root.
     */
    private cleanPath(path: string): string {
        if (typeof path !== 'string') {
            throw new Error('[PS3Z] Ruta no válida.');
        }

        const normalized = path.replace(/\\/g, '/');

        if (
            normalized === '' ||
            normalized === '/' ||
            normalized === '.' ||
            normalized === './'
        ) {
            return '';
        }

        const clean = normalized.replace(/^\/+|\/+$/g, '');

        if (
            !clean ||
            clean.includes('\0') ||
            clean.split('/').some(part =>
                !part || part === '.' || part === '..'
            )
        ) {
            throw new Error(`[PS3Z] Ruta no segura: ${path}`);
        }

        return clean;
    }

    /**
     * Gets file/directory metadata without loading file contents.
     */
    getPathInfo(path: string): Ps3zPathInfo {
        let clean: string;

        try {
            clean = this.cleanPath(path);
        } catch {
            return {
                exists: false,
                isDirectory: false,
                size: 0,
            };
        }

        if (!clean) {
            return {
                exists: true,
                isDirectory: true,
                size: 0,
            };
        }

        const file = this.reader.getFile(clean);

        if (file) {
            return {
                exists: true,
                isDirectory: false,
                size: file.size,
            };
        }

        if (this.reader.isDirectory(clean)) {
            return {
                exists: true,
                isDirectory: true,
                size: 0,
            };
        }

        return {
            exists: false,
            isDirectory: false,
            size: 0,
        };
    }

    /**
     * Returns the number of direct children of a directory.
     */
    getDirectoryEntryCount(path: string): number {
        return this.getDirectoryEntries(path).length;
    }

    /**
     * Returns one direct child of a directory.
     */
    getDirectoryEntry(
        path: string,
        index: number
    ): Ps3zDirectoryEntry | undefined {
        if (!Number.isSafeInteger(index) || index < 0) {
            return undefined;
        }

        return this.getDirectoryEntries(path)[index];
    }

    private getDirectoryEntries(path: string): Ps3zDirectoryEntry[] {
        const clean = this.cleanPath(path);
        const key = clean.toLowerCase();

        const cached = this.directoryCache.get(key);

        if (cached) {
            return cached;
        }

        const entries = this.reader.listDirectory(clean);

        this.directoryCache.set(key, entries);

        return entries;
    }

    /**
     * Starts an asynchronous read into the WASM heap.
     *
     * The C++ side will wait until isDone() returns true.
     * Offset is a JavaScript safe integer, reconstructed by C++ from
     * its low/high 32-bit halves.
     */
    read(
        destinationPointer: number,
        path: string,
        offset: number,
        size: number
    ): void {
        if (
            !Number.isSafeInteger(destinationPointer) ||
            !Number.isSafeInteger(offset) ||
            !Number.isSafeInteger(size) ||
            offset < 0 ||
            size < 0 ||
            size > 0xFFFFFFFF
        ) {
            this.lastError = '[PS3Z] Parámetros de lectura no válidos.';
            this.doneFlag = true;
            return;
        }

        if (size === 0) {
            this.lastError = '';
            this.doneFlag = true;
            return;
        }

        const cleanPath = this.cleanPath(path);
        const destination = destinationPointer >>> 0;

        this.lastError = '';
        this.doneFlag = false;

        this.reader.readFileRange(cleanPath, offset, size)
            .then((bytes: Uint8Array) => {
                const heap = this.module.HEAPU8;

                if (!heap || destination + bytes.length > heap.length) {
                    throw new Error('[PS3Z] La lectura queda fuera de la memoria WASM.');
                }

                heap.set(bytes, destination);
            })
            .catch((error: unknown) => {
                this.lastError = String(error);
            })
            .finally(() => {
                this.doneFlag = true;
            });
    }

    isDone(): boolean {
        return this.doneFlag;
    }

    getLastError(): string {
        return this.lastError;
    }

    clearDirectoryCache(): void {
        this.directoryCache.clear();
    }
}
