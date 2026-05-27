#pragma once

// fs::file_base subclass that adapts the pure-C++ JsDiscBridge into the
// fs:: layer the rpcs3 emucore uses for disc I/O. Only compiled when
// the emucore is linked into the WASM build (HAS_EMUCORE in
// rpcs3/ui_js/CMakeLists.txt) — the standalone scaffold uses
// JsDiscBridge directly for diagnostic reads.
//
// Once emucore is in the build, Rpcs3VmJs::BootDiscImage hands one of
// these to fs::file or to a virtual device's open() so that
// Emu.BootGame(path, "", true) (see main_window::Boot, the seam
// described in plan-poc-ps3-webgpu.md) finds an ISO at `path` whose
// reads stream through the user's File handle on the JS side rather
// than being copied into the WASM heap.
//
// PS3 disc decryption (iso_file_decryption in Loader/ISO.cpp) layers
// on top of fs::file_base, so the encrypted bytes the JS side serves
// pass through unchanged — no extra crypto plumbing on this side.

#ifdef HAS_EMUCORE

#include "Utilities/File.h"
#include "JsDiscBridge.h"

class JsDiscFile final : public fs::file_base
{
public:
	JsDiscFile();
	~JsDiscFile() override = default;

	fs::stat_t get_stat() override;
	bool trunc(u64 length) override;
	u64 read(void* buffer, u64 size) override;
	u64 read_at(u64 offset, void* buffer, u64 size) override;
	u64 write(const void* buffer, u64 size) override;
	u64 seek(s64 offset, fs::seek_mode whence) override;
	u64 size() override;

private:
	JsDiscBridge m_bridge;
	u64 m_pos = 0;
	u64 m_cachedSize = 0;
	bool m_sizeKnown = false;

	u64 ResolveSize();
};

#endif // HAS_EMUCORE
