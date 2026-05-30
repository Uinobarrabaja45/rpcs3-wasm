#!/usr/bin/env python3
"""
PS3UPDAT.PUP → dev_flash/ extractor.

Mirrors rpcs3's firmware-install path:
  1. Parse PUP header (rpcs3/Loader/PUP.h schema).
  2. Pull entry id 0x300 — the master TAR of update_files.
  3. From that master TAR, pick TAR entries whose names start with
     "dev_flash_". Each of those is itself a (nested) TAR.
  4. Extract those nested TARs into the output dev_flash/ directory.

Usage:
    python3 scripts/extract_pup.py PS3UPDAT.PUP out/dev_flash/

Notes:
  - The PUP/TAR formats are simple enough that we don't need rpcs3's
    binaries. Standard library `tarfile` handles both the outer and
    nested TARs.
  - Only the un-encrypted entries land in dev_flash. The SELF/SPRX files
    are still SCE-encrypted but rpcs3 unwraps them at runtime via its
    decryption module, so we don't need to touch encryption here.
"""

import io
import os
import struct
import sys
import tarfile
from pathlib import Path

# PUPHeader (rpcs3/Loader/PUP.h):
#   le_t<u64> magic            — "SCEUF\0\0\0" (LE — i.e. raw bytes "SCEUF\0\0\0")
#   be_t<u64> package_version
#   be_t<u64> image_version
#   be_t<u64> file_count
#   be_t<u64> header_length
#   be_t<u64> data_length
PUP_HEADER = struct.Struct(">8s5Q")  # 8-byte magic + 5 BE u64s

# PUPFileEntry:
#   be_t<u64> entry_id
#   be_t<u64> data_offset
#   be_t<u64> data_length
#   u8[8]     padding
PUP_FILE_ENTRY = struct.Struct(">QQQ8s")

UPDATE_FILES_ENTRY_ID = 0x300


def parse_pup(pup_path: Path):
    """Yield (entry_id, bytes) for each file inside the PUP."""
    data = pup_path.read_bytes()
    magic, pkg_ver, img_ver, file_count, header_len, data_len = PUP_HEADER.unpack_from(data, 0)
    if magic != b"SCEUF\x00\x00\x00":
        raise SystemExit(f"Bad PUP magic: {magic!r}")

    print(f"  PUP package_version=0x{pkg_ver:016x} image_version=0x{img_ver:016x}")
    print(f"  files={file_count} header_len={header_len} data_len={data_len}")

    # Skip 32-byte file table header? rpcs3's pup_object reads:
    #   header (48) + N × PUPFileEntry(32) + N × PUPHashEntry(32)
    # so file entries start at offset 48.
    off = PUP_HEADER.size
    entries = []
    for _ in range(file_count):
        entry_id, data_offset, data_length, _pad = PUP_FILE_ENTRY.unpack_from(data, off)
        entries.append((entry_id, data_offset, data_length))
        off += PUP_FILE_ENTRY.size

    for entry_id, data_offset, data_length in entries:
        yield entry_id, data[data_offset : data_offset + data_length]


def extract_dev_flash(pup_path: Path, out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)

    update_files_bytes = None
    for entry_id, blob in parse_pup(pup_path):
        if entry_id == UPDATE_FILES_ENTRY_ID:
            update_files_bytes = blob
            break

    if update_files_bytes is None:
        raise SystemExit("No entry 0x300 (update_files.tar) in PUP — firmware looks malformed.")

    print(f"  update_files.tar size={len(update_files_bytes)} bytes")

    extracted = 0
    with tarfile.open(fileobj=io.BytesIO(update_files_bytes), mode="r|") as outer:
        for member in outer:
            if not member.name.startswith("dev_flash_") or not member.isfile():
                continue
            nested = outer.extractfile(member)
            if nested is None:
                continue
            print(f"  → unpacking nested TAR {member.name} ({member.size} bytes)")
            nested_data = nested.read()
            try:
                with tarfile.open(fileobj=io.BytesIO(nested_data), mode="r|") as inner:
                    for f in inner:
                        if not f.isfile():
                            continue
                        dst = out_dir / f.name.lstrip("/")
                        dst.parent.mkdir(parents=True, exist_ok=True)
                        with inner.extractfile(f) as src, open(dst, "wb") as out:
                            out.write(src.read())
                        extracted += 1
            except tarfile.ReadError as e:
                print(f"    (skipping {member.name}: {e})")

    return extracted


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} PS3UPDAT.PUP out/dev_flash/", file=sys.stderr)
        sys.exit(2)

    pup_path = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])

    if not pup_path.is_file():
        raise SystemExit(f"PUP not found: {pup_path}")

    print(f"Extracting {pup_path} → {out_dir} …")
    count = extract_dev_flash(pup_path, out_dir)
    print(f"Done. {count} files written.")


if __name__ == "__main__":
    main()
