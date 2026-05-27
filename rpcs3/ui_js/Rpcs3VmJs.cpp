#include "Rpcs3VmJs.h"

#include <cstdio>

// Implementation mirrors rpcs3qt/main_window.cpp's bootIsoAct → BootISO →
// Boot chain, minus the Qt dialog / gui_settings persistence. The chain is:
//
//   1. main_window::BootISO()                       ← user action
//        ↓
//   2. main_window::Boot(path, "", true, true)      ← shared boot driver
//        ↓
//        ├── Emu.IsBootingRestricted() guard         (skipped in browser — no nested boots)
//        ├── m_gui_settings GetBootConfirmation       (skipped — no Qt dialog)
//        ├── Emu.GracefulShutdown(false)              ← we log this
//        ├── m_game_list_frame->GetConfigDatabase()   (skipped — no game list cache)
//        ├── Emu.BootGame(path, title_id, direct, ...) ← we log this
//        └── (success) AddRecentAction (skipped — no recent-list UI)
//
// In a full integration build, the printf statements below are replaced
// with the real Emu.* calls. For the POC the stubs let the JS side observe
// the same lifecycle (status transitions, [Rpcs3VmJs] BootDiscImage prints)
// so the React UI and the screenshot pipeline already work against the
// shape of the real flow.

#ifdef HAS_EMUCORE
#include "Emu/System.h"
#include "Emu/system_config_types.h"
#endif

CRpcs3VmJs* g_virtualMachine = nullptr;

CRpcs3VmJs::CRpcs3VmJs()
{
	printf("[Rpcs3VmJs] constructed\n");
}

CRpcs3VmJs::~CRpcs3VmJs()
{
	printf("[Rpcs3VmJs] destroyed\n");
}

void CRpcs3VmJs::CreateVM()
{
	printf("[Rpcs3VmJs] CreateVM — registering JIT runtime helpers\n");
	// In the full build this is where Emu.SetCallbacks(...) lives,
	// mirroring headless_application::InitializeCallbacks() but with
	// WebGPU-friendly substitutions for init_gs_render / get_gs_frame.
	// See plan-poc-ps3-webgpu.md §2.4 for the threading map (PPU + 6 SPU
	// + RSX + IOP across pthread workers) that has to be set up here.
}

void CRpcs3VmJs::Boot(const std::string& path, const std::string& title_id, bool direct)
{
	// Mirrors main_window::Boot — log, GracefulShutdown, then BootGame.
	// gui_log.notice in the Qt build becomes a printf here so the JS side
	// can pattern-match the "Booting..." string for screenshot triggers.
	printf("[Rpcs3VmJs] Boot path='%s' title_id='%s' direct=%d\n",
	       path.c_str(), title_id.c_str(), direct ? 1 : 0);

	m_status = Status::Booting;

#ifdef HAS_EMUCORE
	Emu.GracefulShutdown(false);
	printf("[Rpcs3VmJs] Emu.GracefulShutdown(false) returned\n");

	const auto error = Emu.BootGame(path, title_id, direct);
	if (error != game_boot_result::no_errors)
	{
		printf("[Rpcs3VmJs] Emu.BootGame failed: %s\n", fmt::format("%s", error).c_str());
		m_status = Status::Stopped;
		return;
	}
	printf("[Rpcs3VmJs] Emu.BootGame succeeded — path=%s\n", Emu.GetBoot().c_str());
	m_bootPath = Emu.GetBoot();
#else
	// POC scaffold path — emucore not linked. Log the same milestones so
	// downstream code (Stats FPS pill, screenshot triggers, the WebGPU
	// render loop's `if (boot ready)` gate) sees the right state edges.
	printf("[Rpcs3VmJs] [POC] Emu.GracefulShutdown(false) [stub]\n");
	printf("[Rpcs3VmJs] [POC] Emu.BootGame('%s', '%s', %d) [stub]\n",
	       path.c_str(), title_id.c_str(), direct ? 1 : 0);
	m_bootPath = path;
#endif

	m_status = Status::Running;
}

void CRpcs3VmJs::BootElf(std::string path)
{
	printf("[Rpcs3VmJs] BootElf('%s')\n", path.c_str());
	Boot(path, "", /*direct=*/true);
}

void CRpcs3VmJs::BootDiscImage(std::string path)
{
	printf("[Rpcs3VmJs] BootDiscImage('%s')\n", path.c_str());
	// `direct=true` matches main_window::BootISO. The full Boot chain
	// (Emu.BootGame) will resolve the path through iso_device when the
	// file is recognised as a PS3 disc image — see Loader/ISO.cpp's
	// iso_archive::mount + Loader/disc.cpp's get_disc_type.
	Boot(path, "", /*direct=*/true);
}

void CRpcs3VmJs::BootXmb()
{
	printf("[Rpcs3VmJs] BootXmb\n");
	// main_window::BootVSH passes g_cfg_vfs.get_dev_flash() +
	// "/vsh/module/vsh.self". Hard-code the standard path here — in the
	// browser dev_flash is OPFS-mounted (plan-poc-ps3-webgpu.md §2.3) so
	// the same string resolves.
	Boot("/dev_flash/vsh/module/vsh.self", "", /*direct=*/false);
}

void CRpcs3VmJs::Pause()
{
	printf("[Rpcs3VmJs] Pause\n");
#ifdef HAS_EMUCORE
	Emu.Pause();
#endif
	if (m_status == Status::Running) m_status = Status::Paused;
}

void CRpcs3VmJs::Resume()
{
	printf("[Rpcs3VmJs] Resume\n");
#ifdef HAS_EMUCORE
	Emu.Resume();
#endif
	if (m_status == Status::Paused) m_status = Status::Running;
}

void CRpcs3VmJs::Stop()
{
	printf("[Rpcs3VmJs] Stop\n");
#ifdef HAS_EMUCORE
	Emu.GracefulShutdown(false);
#endif
	m_status = Status::Stopped;
	m_bootPath.clear();
}
