/**
 * Random-access reader for the PS3Z folder bundle produced by pack-ps3z.py.
 *
 * PS3Z v1 layout:
 *   [48-byte header][UTF-8 JSON manifest][payload bytes]
 * Header: <8sII32s = magic, version, manifest byte length, SHA-256(manifest).
 * Chunk offsets in the manifest are relative to the start of the payload.
 * Each chunk is an independent Zstandard frame or an uncompressed raw block.
 *
 * This reader intentionally does not pretend a PS3Z folder bundle is an ISO.
 * It exposes files by path; RPCS3 still needs a virtual-filesystem adapter to
 * boot a directory-based game directly from this reader.
 */
import { decompress } from 'fzstd';

const HEADER_SIZE = 48;
const MAGIC = 'PS3ZPK01';
const MAX_MANIFEST_BYTES = 64 * 1024 * 1024;
const MAX_CHUNK_BYTES = 2 * 1024 * 1024;
const DEFAULT_CACHE_BYTES = 32 * 1024 * 1024;
const SHA256_RE = /^[0-9a-f]{64}$/i;

export type Ps3zCodec = 'raw' | 'zstd';

export interface Ps3zChunk {
  offset: number;
  compressedSize: number;
  size: number;
  codec: Ps3zCodec;
  sha256: string;
}

export interface Ps3zFileEntry {
  path: string;
  size: number;
  mtime?: number;
  chunks: Ps3zChunk[];
}

export interface Ps3zDirectoryEntry {
  name: string;
  path: string;
  isDirectory: boolean;
  size: number;
}

interface Ps3zManifest {
  format: 'PS3Z';
  version: 1;
  compression: string;
  rootName: string;
  chunkSize: number;
  totalFiles: number;
  totalBytes: number;
  createdAt?: string;
  compressionLevel?: number;
  files: Ps3zFileEntry[];
}

export interface Ps3zReaderOptions {
  /** Maximum uncompressed block data kept in the LRU cache. Default: 32 MiB. */
  cacheBytes?: number;
  /** Verify every block's SHA-256 before returning it. Default: true. */
  verifyChunks?: boolean;
}

function fail(message: string): never {
  throw new Error(`[PS3Z] ${message}`);
}

function isSafeInteger(value: unknown): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0;
}

function normalizeLookupPath(input: string): string {
  if (typeof input !== 'string' || !input) fail('La ruta está vacía.');
  const normalized = input.replace(/\\/g, '/');
  if (normalized.startsWith('/') || normalized.includes('\0')) {
    fail(`Ruta absoluta o no válida: ${input}`);
  }
  const parts = normalized.split('/');
  if (parts.some(part => !part || part === '.' || part === '..')) {
    fail(`Ruta no segura: ${input}`);
  }
  return parts.join('/');
}

function bytesToHex(bytes: Uint8Array): string {
  let result = '';
  for (let i = 0; i < bytes.length; i++) result += bytes[i].toString(16).padStart(2, '0');
  return result;
}

async function sha256Hex(bytes: Uint8Array): Promise<string> {
  if (!globalThis.crypto || !globalThis.crypto.subtle) {
    fail('Este navegador no ofrece Web Crypto. Abre la web mediante HTTPS.');
  }
  // Copy into a standalone ArrayBuffer because SubtleCrypto's TS overloads
  // differ between older TypeScript DOM libraries and newer browsers.
  const input = new Uint8Array(bytes.length);
  input.set(bytes);
  const digest = await globalThis.crypto.subtle.digest('SHA-256', input.buffer);
  return bytesToHex(new Uint8Array(digest));
}

async function readBlob(blob: Blob, start: number, end: number): Promise<Uint8Array> {
  const part = blob.slice(start, end);
  const bytes = new Uint8Array(await part.arrayBuffer());
  const expected = end - start;
  if (bytes.length !== expected) {
    fail(`Lectura incompleta: se esperaban ${expected} bytes y llegaron ${bytes.length}.`);
  }
  return bytes;
}

function validateManifest(value: any, payloadLength: number): Ps3zManifest {
  if (!value || typeof value !== 'object' || Array.isArray(value)) fail('El índice JSON no es un objeto.');
  if (value.format !== 'PS3Z' || value.version !== 1) fail('Formato o versión PS3Z no compatibles.');
  if (value.compression !== 'zstd-chunked-v1') fail('Método de compresión no compatible.');
  if (typeof value.rootName !== 'string' || !value.rootName) fail('Falta rootName en el índice.');
  if (!isSafeInteger(value.chunkSize) || value.chunkSize < 1 || value.chunkSize > MAX_CHUNK_BYTES) {
    fail('Tamaño de bloque no válido (máximo 2 MiB).');
  }
  if (!Array.isArray(value.files) || value.files.length > 200000) fail('Lista de archivos no válida.');
  if (value.totalFiles !== value.files.length) fail('totalFiles no coincide con el índice.');

  const seenPaths = new Set<string>();
  let byteTotal = 0;
  let expectedPayloadOffset = 0;

  for (const rawEntry of value.files) {
    if (!rawEntry || typeof rawEntry !== 'object') fail('Entrada de archivo no válida.');
    if (typeof rawEntry.path !== 'string') fail('Una entrada no tiene ruta.');
    const path = normalizeLookupPath(rawEntry.path);
    if (path !== rawEntry.path) fail(`La ruta del índice no está normalizada: ${rawEntry.path}`);
    const key = path.toLowerCase();
    if (seenPaths.has(key)) fail(`Ruta duplicada ignorando mayúsculas: ${path}`);
    seenPaths.add(key);
    if (!isSafeInteger(rawEntry.size)) fail(`Tamaño no válido: ${path}`);
    if (!Array.isArray(rawEntry.chunks)) fail(`Lista de bloques no válida: ${path}`);

    let reconstructedSize = 0;
    for (const chunk of rawEntry.chunks as any[]) {
      if (!chunk || typeof chunk !== 'object') fail(`Bloque no válido: ${path}`);
      if (!isSafeInteger(chunk.offset) || !isSafeInteger(chunk.compressedSize) || !isSafeInteger(chunk.size)) {
        fail(`Offset o tamaño no válido en ${path}`);
      }
      if (chunk.compressedSize < 1 || chunk.size < 1 || chunk.compressedSize > chunk.size || chunk.size > value.chunkSize || chunk.size > MAX_CHUNK_BYTES) {
        fail(`Tamaño de bloque fuera de límites en ${path}`);
      }
      if (chunk.offset !== expectedPayloadOffset) {
        fail(`Índice discontinuo o solapado en el bloque ${path} (offset ${chunk.offset}, esperado ${expectedPayloadOffset}).`);
      }
      if (chunk.offset + chunk.compressedSize > payloadLength) fail(`Bloque fuera del archivo PS3Z: ${path}`);
      if (chunk.codec !== 'raw' && chunk.codec !== 'zstd') fail(`Codec desconocido en ${path}`);
      if (chunk.codec === 'raw' && chunk.compressedSize !== chunk.size) fail(`Bloque raw con tamaño incoherente en ${path}`);
      if (typeof chunk.sha256 !== 'string' || !SHA256_RE.test(chunk.sha256)) fail(`SHA-256 no válido en ${path}`);
      expectedPayloadOffset += chunk.compressedSize;
      reconstructedSize += chunk.size;
    }
    if (reconstructedSize !== rawEntry.size) fail(`La suma de bloques no coincide con el tamaño de ${path}`);
    byteTotal += rawEntry.size;
    if (!Number.isSafeInteger(byteTotal)) fail('El tamaño total supera el límite seguro de JavaScript.');
  }

  if (byteTotal !== value.totalBytes) fail('totalBytes no coincide con los tamaños de archivo.');
  if (expectedPayloadOffset !== payloadLength) fail('El tamaño de los bloques no coincide con la zona de datos del PS3Z.');
  return value as Ps3zManifest;
}

/** Opens and validates a PS3Z file without reading its payload into memory. */
export default class Ps3zReader {
  readonly file: Blob;
  readonly rootName: string;
  readonly totalFiles: number;
  readonly totalBytes: number;
  readonly chunkSize: number;
  readonly payloadOffset: number;
  readonly files: readonly Ps3zFileEntry[];

  private readonly entries = new Map<string, Ps3zFileEntry>();
  private readonly cache = new Map<string, Uint8Array>();
  private readonly pending = new Map<string, Promise<Uint8Array>>();
  private readonly cacheLimit: number;
  private readonly verifyChunks: boolean;
  private cachedBytes = 0;

  private constructor(file: Blob, manifest: Ps3zManifest, payloadOffset: number, options: Ps3zReaderOptions) {
    this.file = file;
    this.rootName = manifest.rootName;
    this.totalFiles = manifest.totalFiles;
    this.totalBytes = manifest.totalBytes;
    this.chunkSize = manifest.chunkSize;
    this.payloadOffset = payloadOffset;
    this.files = manifest.files;
    this.cacheLimit = Math.max(0, options.cacheBytes ?? DEFAULT_CACHE_BYTES);
    this.verifyChunks = options.verifyChunks !== false;
    for (const entry of manifest.files) this.entries.set(entry.path.toLowerCase(), entry);
  }

  static async open(file: Blob, options: Ps3zReaderOptions = {}): Promise<Ps3zReader> {
    if (!file || typeof file.slice !== 'function' || !Number.isSafeInteger(file.size)) fail('No se ha seleccionado un archivo válido.');
    if (file.size < HEADER_SIZE) fail('Archivo demasiado pequeño para ser PS3Z.');

    const header = await readBlob(file, 0, HEADER_SIZE);
    const magic = new TextDecoder('ascii').decode(header.subarray(0, 8));
    if (magic !== MAGIC) fail('Cabecera incorrecta: no es un archivo PS3Z.');

    const view = new DataView(header.buffer, header.byteOffset, header.byteLength);
    const version = view.getUint32(8, true);
    const manifestLength = view.getUint32(12, true);
    if (version !== 1) fail(`Versión PS3Z no compatible: ${version}`);
    if (manifestLength < 2 || manifestLength > MAX_MANIFEST_BYTES) fail('Tamaño del índice fuera de límites.');
    const payloadOffset = HEADER_SIZE + manifestLength;
    if (payloadOffset > file.size) fail('El índice termina fuera del archivo.');

    const manifestBytes = await readBlob(file, HEADER_SIZE, payloadOffset);
    const expectedHash = bytesToHex(header.subarray(16, 48));
    const actualHash = await sha256Hex(manifestBytes);
    if (actualHash !== expectedHash) fail('El SHA-256 del índice no coincide. El PS3Z está dañado.');

    let parsed: unknown;
    try {
      parsed = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(manifestBytes));
    } catch (error) {
      fail(`No se puede interpretar el índice JSON: ${String(error)}`);
    }

    const manifest = validateManifest(parsed, file.size - payloadOffset);
    return new Ps3zReader(file, manifest, payloadOffset, options);
  }

  getFile(path: string): Ps3zFileEntry | undefined {
    return this.entries.get(normalizeLookupPath(path).toLowerCase());
  }

  findFilesByName(name: string): Ps3zFileEntry[] {
    const wanted = name.toLowerCase();
    return this.files.filter(entry => entry.path.split('/').pop()!.toLowerCase() === wanted);
  }

  listFiles(prefix = ''): Ps3zFileEntry[] {
    const cleanPrefix = prefix ? normalizeLookupPath(prefix).toLowerCase() + '/' : '';
    return this.files.filter(entry => entry.path.toLowerCase().startsWith(cleanPrefix));
  }

  /**
   * Read a byte range from one file in the PS3Z bundle. Only intersecting
   * blocks are fetched and decompressed. Ranges may cross block boundaries.
   */
  async readFileRange(path: string, offset: number, length: number): Promise<Uint8Array> {
    const entry = this.getFile(path);
    if (!entry) fail(`No existe el archivo: ${path}`);
    if (!isSafeInteger(offset) || !isSafeInteger(length)) fail('Offset o longitud no válidos.');
    if (offset > entry.size || offset + length > entry.size || !Number.isSafeInteger(offset + length)) {
      fail(`Lectura fuera de los límites de ${path}.`);
    }
    if (length === 0) return new Uint8Array(0);

    const result = new Uint8Array(length);
    const end = offset + length;
    let fileChunkStart = 0;
    let outputOffset = 0;

    for (let index = 0; index < entry.chunks.length && fileChunkStart < end; index++) {
      const chunk = entry.chunks[index];
      const fileChunkEnd = fileChunkStart + chunk.size;
      if (fileChunkEnd <= offset) {
        fileChunkStart = fileChunkEnd;
        continue;
      }
      if (fileChunkStart >= end) break;

      const data = await this.loadChunk(entry, index);
      const sourceStart = Math.max(0, offset - fileChunkStart);
      const sourceEnd = Math.min(chunk.size, end - fileChunkStart);
      const count = sourceEnd - sourceStart;
      result.set(data.subarray(sourceStart, sourceEnd), outputOffset);
      outputOffset += count;
      fileChunkStart = fileChunkEnd;
    }

    if (outputOffset !== length) fail(`Lectura incompleta de ${path}: ${outputOffset}/${length} bytes.`);
    return result;
  }

  /** Convenience method for small files (for example EBOOT.BIN headers). */
  async readFile(path: string, maxBytes = 512 * 1024 * 1024): Promise<Uint8Array> {
    const entry = this.getFile(path);
    if (!entry) fail(`No existe el archivo: ${path}`);
    if (entry.size > maxBytes) fail(`El archivo es demasiado grande para leerlo entero (${entry.size} bytes). Usa readFileRange().`);
    return this.readFileRange(path, 0, entry.size);
  }

  getCachedBytes(): number {
    return this.cachedBytes;
  }

  clearCache(): void {
    this.cache.clear();
    this.cachedBytes = 0;
  }

  private async loadChunk(entry: Ps3zFileEntry, index: number): Promise<Uint8Array> {
    const key = `${entry.path.toLowerCase()}#${index}`;
    const cached = this.cache.get(key);
    if (cached) {
      // Refresh LRU position.
      this.cache.delete(key);
      this.cache.set(key, cached);
      return cached;
    }
    const pending = this.pending.get(key);
    if (pending) return pending;

    const chunk = entry.chunks[index];
    const operation = (async () => {
      const start = this.payloadOffset + chunk.offset;
      const stored = await readBlob(this.file, start, start + chunk.compressedSize);
      let decoded: Uint8Array;
      if (chunk.codec === 'raw') {
        decoded = stored;
      } else {
        try {
          decoded = decompress(stored);
        } catch (error) {
          fail(`Zstandard no pudo descomprimir ${entry.path} bloque ${index}: ${String(error)}`);
        }
      }
      if (decoded.length !== chunk.size) {
        fail(`Tamaño descomprimido incorrecto en ${entry.path} bloque ${index}: ${decoded.length}/${chunk.size}.`);
      }
      if (this.verifyChunks) {
        const actualHash = await sha256Hex(decoded);
        if (actualHash !== chunk.sha256.toLowerCase()) {
          fail(`SHA-256 incorrecto en ${entry.path} bloque ${index}.`);
        }
      }
      if (this.cacheLimit >= decoded.length && decoded.length > 0) {
        this.cache.set(key, decoded);
        this.cachedBytes += decoded.length;
        while (this.cachedBytes > this.cacheLimit && this.cache.size > 0) {
          const oldestKey = this.cache.keys().next().value as string;
          const oldest = this.cache.get(oldestKey)!;
          this.cache.delete(oldestKey);
          this.cachedBytes -= oldest.length;
        }
      }
      return decoded;
    })();
    this.pending.set(key, operation);
    try {
      return await operation;
    } finally {
      this.pending.delete(key);
    }
  }
}
