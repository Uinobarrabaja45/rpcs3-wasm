#pragma once

#include <cstdint>
#include <mutex>

// JS-disc-device bridge — pure C++ wrapper around the JS-side
// DiscImageDevice that gets attached at Module.discImageDevice. Doesn't
// touch rpcs3's fs:: layer so it can compile in the standalone (no-
// emucore) build; the fs::file_base subclass that the emulator actually
// hands to BootGame lives in JsDiscFile.{h,cpp} and is guarded by the
// emucore presence (see CMakeLists.txt).
//
// Threading: the bridge is safe to call from any pthread worker.
// MAIN_THREAD_EM_ASM proxies the JS call to the DOM thread (where
// Module.discImageDevice was attached) and blocks the caller until the
// main-thread runtime returns. A mutex serialises overlapping calls so
// the JS doneFlag — which is a single bool — is never raced.
//
// Lifetime: stateless singleton. The bridge holds no pointer to the
// JS-side device; it looks it up by name on every call. That keeps it
// resilient across device.lost / module-restart events (the JS shell
// re-attaches Module.discImageDevice on recovery; the bridge picks up
// the new instance on the next call without restart).

class JsDiscBridge
{
public:
	JsDiscBridge() = default;

	// Read `size` bytes starting at `offset` into `dst`. Blocks the
	// calling thread until the JS slice resolves. Returns the number of
	// bytes actually copied (always equal to `size` on success — JS
	// File.slice never short-reads within bounds), or 0 if the JS side
	// has no file attached or the request is otherwise rejected.
	uint64_t ReadAt(uint64_t offset, void* dst, uint64_t size);

	// Total byte size of the file currently attached to
	// Module.discImageDevice. Returns 0 when no file is set, so the
	// caller can treat "no file" as "0-byte file" rather than having
	// to special-case it.
	uint64_t GetSize();

	// Whether Module.discImageDevice has a File attached. Cheap; just
	// reads the `file` property. Useful as a precondition check before
	// the emulator tries to boot from the device.
	bool HasFile();

private:
	std::mutex m_callMutex;
};
