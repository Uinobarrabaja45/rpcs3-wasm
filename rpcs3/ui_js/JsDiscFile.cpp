#include "JsDiscFile.h"

#ifdef HAS_EMUCORE

#include <cstdio>

// fs::file_base adapter — delegates all reads to JsDiscBridge, which
// proxies into the JS DiscImageDevice. See header for the integration
// contract; this file is compiled only when the rpcs3 emucore is in
// the build.

JsDiscFile::JsDiscFile()
{
	printf("[JsDiscFile] constructed\n");
}

fs::stat_t JsDiscFile::get_stat()
{
	fs::stat_t st = {};
	st.is_directory = false;
	st.is_symlink   = false;
	st.is_writable  = false;
	st.size         = ResolveSize();
	// PS3 ISOs are read-only inputs to the emulator; leaving the time
	// fields as zero is fine — the emu's only consumer of these fields
	// is the game-list dialog, which we don't run in the browser.
	return st;
}

bool JsDiscFile::trunc(u64 /*length*/)
{
	return false;
}

u64 JsDiscFile::read(void* buffer, u64 size)
{
	const u64 got = m_bridge.ReadAt(m_pos, buffer, size);
	m_pos += got;
	return got;
}

u64 JsDiscFile::read_at(u64 offset, void* buffer, u64 size)
{
	// `read_at` is the positional-read fast path; doesn't move the
	// cursor. The bridge already takes an explicit offset, so this is
	// a direct passthrough.
	return m_bridge.ReadAt(offset, buffer, size);
}

u64 JsDiscFile::write(const void* /*buffer*/, u64 /*size*/)
{
	return 0;
}

u64 JsDiscFile::seek(s64 offset, fs::seek_mode whence)
{
	const u64 sz = ResolveSize();
	u64 newPos = 0;
	switch (whence)
	{
	case fs::seek_set: newPos = static_cast<u64>(std::max<s64>(0, offset)); break;
	case fs::seek_cur: newPos = m_pos + static_cast<u64>(offset); break;
	case fs::seek_end: newPos = sz + static_cast<u64>(offset); break;
	}
	m_pos = std::min(newPos, sz);
	return m_pos;
}

u64 JsDiscFile::size()
{
	return ResolveSize();
}

u64 JsDiscFile::ResolveSize()
{
	if (!m_sizeKnown)
	{
		m_cachedSize = m_bridge.GetSize();
		m_sizeKnown = m_cachedSize > 0;
	}
	return m_cachedSize;
}

#endif // HAS_EMUCORE
