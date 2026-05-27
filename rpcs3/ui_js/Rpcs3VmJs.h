#pragma once

#include <string>

// Browser-facing wrapper around RPCS3's emu core (Emu/System.h).
//
// Mirrors Play!'s CPs2VmJs: a thin subclass that
//   1. registers JIT runtime helpers in the WASM function registry
//   2. exposes Boot* helpers that funnel through the engine's mailbox so
//      the heavy work happens on the dedicated emu thread, not the main UI
//      thread (which would freeze the browser tab).
//
// BOOT FLOW (mirrors rpcs3qt/main_window.cpp's bootIsoAct → BootISO →
// Boot → Emu.BootGame chain):
//
//   bootIsoAct.triggered  →  main_window::BootISO()
//                              ↓
//                            main_window::Boot(path, "", direct=true, ...)
//                              ↓ (gui_log.notice + GracefulShutdown(false))
//                              ↓
//                            Emu.BootGame(path, title_id, direct, config_mode,
//                                         config_path, db_config)
//
// In the browser the same call sequence runs without the Qt dialogs —
// `path` is the MEMFS filename that DiscImageDevice will stream from a JS
// File object. Once emucore is linked into the WASM build (Phase 2 in
// plan-poc-ps3-webgpu.md), the printf stubs in Rpcs3VmJs.cpp become real
// Emu.* calls. The wrapper class shape stays unchanged — calling code
// (Main.cpp embind exports, Actions.ts) is decoupled from the
// scaffold-vs-real-emucore choice.
class CRpcs3VmJs
{
public:
	enum class Status
	{
		Stopped,
		Booting,
		Running,
		Paused,
	};

	CRpcs3VmJs();
	~CRpcs3VmJs();

	// Wires the WASM JIT runtime helpers and Emu singletons. Equivalent
	// to the parts of main_application::InitializeCallbacks() that don't
	// depend on Qt.
	void CreateVM();

	// (S)ELF path inside MEMFS — homebrew, demos, debug ELFs from the
	// game disc. Mirrors main_window::BootElf → Boot(path, "", true, true).
	void BootElf(std::string path);

	// PS3 disc image (.iso / .bin / .pkg). `path` is a virtual MEMFS name
	// the host-side DiscImageDevice services on demand (no full copy into
	// the WASM heap). Mirrors main_window::BootISO → Boot(path, "", true,
	// true) which then calls Emu.GracefulShutdown(false) + Emu.BootGame.
	void BootDiscImage(std::string path);

	// Boot the XMB / firmware front-end (vsh.self). Requires firmware to
	// have been installed into the OPFS-backed dev_flash mount. Mirrors
	// main_window::BootVSH → Boot(g_cfg_vfs.get_dev_flash() +
	// "/vsh/module/vsh.self").
	void BootXmb();

	void Pause();
	void Resume();
	void Stop();

	bool IsRunning() const { return m_status == Status::Running; }
	Status GetStatus() const { return m_status; }

	// Last successfully-booted path. Empty if nothing's been booted.
	// Mirrors Emu.GetBoot().
	const std::string& GetBoot() const { return m_bootPath; }

private:
	// Shared by BootElf / BootDiscImage / BootXmb — the per-call differences
	// (direct flag, default config path) are passed in. Mirrors
	// main_window::Boot() except without GUI confirmation dialogs.
	void Boot(const std::string& path, const std::string& title_id, bool direct);

	Status      m_status = Status::Stopped;
	std::string m_bootPath;
};

extern CRpcs3VmJs* g_virtualMachine;
