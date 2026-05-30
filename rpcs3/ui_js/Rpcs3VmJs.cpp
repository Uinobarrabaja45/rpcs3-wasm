#include "Rpcs3VmJs.h"

#include <cstdio>
#include <cstdint>
#include <thread>

#include "JsDiscBridge.h"

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
#include "Emu/system_config.h"
#include "Emu/system_config_types.h"
#include "Emu/vfs_config.h"
#include "Utilities/File.h"
#include "JsDiscFile.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Input/pad_thread.h"
#include "WebGPUGSRender.h"
#include "util/fixed_typemap.hpp"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "util/video_source.h"

namespace fs {
	extern std::unique_ptr<file_base> (*g_emscripten_open_disc_file)();
}
extern std::string g_booted_disc_path;

#include "util/logs.hpp"

struct JseConsoleLogListener final : logs::listener
{
	void log(u64 /*stamp*/, const logs::message& msg, std::string_view prefix, std::string_view text) override
	{
		// Drop low-severity spam (notice/trace) on wasm. The early-boot
		// ppu_loader import/export flood is THOUSANDS of notice logs, each
		// costing a slow emscripten->console->CDP round-trip that throttles
		// the boot AND bloats the headless renderer (a prime suspect for the
		// flaky ~3.5-8min "Debugger connection closed" before the slow but
		// finite boot reaches its first flip). Keep warning/error/fatal/always.
		if (static_cast<logs::level>(msg) > logs::level::warning)
			return;
		std::string ch_name = msg->name ? msg->name : "";
		printf("[%s] %.*s: %.*s\n", ch_name.c_str(), (int)prefix.size(), prefix.data(), (int)text.size(), text.data());
	}
};
static JseConsoleLogListener g_consoleLogListener;

struct InitDiscFileCallback {
	InitDiscFileCallback() {
		setvbuf(stdout, nullptr, _IONBF, 0);
		setvbuf(stderr, nullptr, _IONBF, 0);
		fs::g_emscripten_open_disc_file = []() -> std::unique_ptr<fs::file_base> {
			return std::make_unique<JsDiscFile>();
		};
		logs::listener::add(&g_consoleLogListener);
	}
} g_initDiscFileCallback;
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

#ifdef HAS_EMUCORE
	// Point dev_flash at the MEMFS path where Rpcs3Module.ts preloads
	// firmware .sprx files. Default is "$(EmulatorDir)dev_flash/" which
	// resolves to a config-dir-relative path on host (/home/web_user/
	// .config/rpcs3/dev_flash/ under Emscripten); our preload writes to
	// /dev_flash/ directly to keep the JS-side code simple.
	g_cfg_vfs.dev_flash.from_string("/dev_flash/");

	// "Potato" config — minimize memory + work for the wasm32 POC. The
	// wasm linear-memory hard cap is 4 GiB (MAXIMUM_MEMORY in
	// rpcs3/ui_js/CMakeLists.txt). The vm::g_base_addr reservation
	// already eats 1.5 GiB at static-init; everything else has to fit in
	// the remaining ~2.5 GiB. Trim every knob that costs RAM or CPU.
	g_cfg.core.ppu_decoder.set(ppu_decoder_type::_static);   // No LLVM JIT (doesn't compile under emcc anyway)
	g_cfg.core.spu_decoder.set(spu_decoder_type::_static);   // Same for SPU
	g_cfg.core.llvm_precompilation.set(false);               // Skip per-game LLVM precompile cache
	g_cfg.core.max_spurs_threads.set(1);                     // Fewer SPU worker threads — each eats a pthread + stack
	g_cfg.video.resolution.set(video_resolution::_480p);     // 480p framebuffer (vs 720p default) → smaller textures
	g_cfg.video.frame_limit.set(frame_limit_type::_30);      // 30fps cap — half the work
	g_cfg.video.disable_vertex_cache.set(true);              // Skip vertex cache → smaller GPU buffers
	g_cfg.audio.renderer.set(audio_renderer::null);          // Already null'd via callback; make it explicit
#endif
	// In the full build this is where Emu.SetCallbacks(...) lives,
	// mirroring headless_application::InitializeCallbacks() but with
	// WebGPU-friendly substitutions for init_gs_render / get_gs_frame.
	// See plan-poc-ps3-webgpu.md §2.4 for the threading map (PPU + 6 SPU
	// + RSX + IOP across pthread workers) that has to be set up here.

#ifdef HAS_EMUCORE
	EmuCallbacks callbacks = {};

	callbacks.call_from_main_thread = [](std::function<void()> func, atomic_t<u32>* wake_up)
	{
		func();
		if (wake_up)
		{
			*wake_up = true;
			wake_up->notify_one();
		}
	};
	callbacks.on_run    = [](bool) {};
	callbacks.on_pause  = []() {};
	callbacks.on_resume = []() {};
	callbacks.on_stop   = []() {};
	callbacks.on_ready  = []() {};
	callbacks.on_missing_fw = []() {};
	callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>>, int) {};
	callbacks.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};
	callbacks.enable_disc_eject  = [](bool) {};
	callbacks.enable_disc_insert = [](bool) {};
	callbacks.try_to_quit = [](bool, std::function<void()>) { return false; };
	callbacks.handle_taskbar_progress = [](s32, s32) {};
	callbacks.init_kb_handler = []() {};
	callbacks.init_mouse_handler = []() {};
	callbacks.init_pad_handler = [](std::string_view title_id)
	{
		// Construct the stub pad_thread defined in EmscriptenStubs.cpp.
		// Its ctor publishes itself into pad::g_pad_thread + fills m_pads
		// with disconnected stub Pads so cellPadInit's connect-event loop
		// becomes a no-op. nullptr curthread/curwindow — no Qt main loop
		// in the browser; input goes through InputProviderEmscripten.
		g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, title_id);
	};
	callbacks.update_emu_settings = []() {};
	callbacks.save_emu_settings = []() {};
	callbacks.close_gs_frame = []() {};
	callbacks.get_gs_frame = []() { return std::unique_ptr<GSFrameBase>(); };
	callbacks.get_camera_handler = []() { return std::shared_ptr<camera_handler_base>(); };
	callbacks.get_music_handler = []() { return std::shared_ptr<music_handler_base>(); };
	callbacks.init_gs_render = [](utils::serial* ar)
	{
		// wasm32 POC: spawn WebGPUGSRender as the rsx::thread fxo singleton.
		// WebGPUGSRender's end() is still a no-op draw (same as NullGSRender)
		// but its flip() bumps g_webgpu_flip_count so the browser-side
		// GSH_WebGPUJs::Render and Main.cpp's mainLoopTick can observe
		// real RSX presents. The BASE rsx::thread runs the full FIFO pump:
		// it pulls cellGcm commands out of guest memory, decodes them,
		// advances the FIFO pointer, and notifies flip-status back to the
		// PPU. That's the bridge Terraria needs to make progress past
		// cellGcmInit.
		//
		// Phase 7+ will replace end()/flip() with WGSL-translated draws +
		// real framebuffer present, but the rsx::thread plumbing is the
		// same — only the backend changes.
		printf("[Rpcs3VmJs] init_gs_render: spawning WebGPUGSRender (wasm32 POC)\n");
		g_fxo->init<rsx::thread, named_thread<WebGPUGSRender>>(ar);
		printf("[Rpcs3VmJs] init_gs_render: rsx::thread initialized\n");
	};
	callbacks.get_audio = []()
	{
		// MUST be non-null — cell_audio_config::reset() does
		// backend->GetName() without a null check, so returning an empty
		// shared_ptr crashes the cellAudio worker with "RuntimeError: null
		// function" on wasm32. NullAudioBackend is a no-op stub (Close/Play
		// /Pause are bools and GetName returns "Null") — perfect for the
		// browser POC where audio is non-essential.
		return std::shared_ptr<AudioBackend>(std::make_shared<NullAudioBackend>());
	};
	callbacks.get_audio_enumerator = [](u64) { return std::shared_ptr<audio_device_enumerator>(); };
	callbacks.get_msg_dialog = []() { return std::shared_ptr<MsgDialogBase>(); };
	callbacks.get_osk_dialog = []() { return std::shared_ptr<OskDialogBase>(); };
	callbacks.get_save_dialog = []() { return std::unique_ptr<SaveDialogBase>(); };
	callbacks.get_sendmessage_dialog = []() { return std::shared_ptr<SendMessageDialogBase>(); };
	callbacks.get_recvmessage_dialog = []() { return std::shared_ptr<RecvMessageDialogBase>(); };
	callbacks.get_trophy_notification_dialog = []() { return std::unique_ptr<TrophyNotificationBase>(); };
	callbacks.get_localized_string = [](localized_string_id, const char*) { return std::string(); };
	callbacks.get_localized_u32string = [](localized_string_id, const char*) { return std::u32string(); };
	callbacks.get_localized_setting = [](const cfg::_base*, u32) { return std::string(); };
	callbacks.get_photo_path = [](std::string_view) { return std::string(); };
	callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
	callbacks.get_image_info = [](const std::string&, std::string&, s32&, s32&, s32&) { return false; };
	callbacks.get_scaled_image = [](const std::string&, s32, s32, s32&, s32&, u8*, bool) { return false; };
	callbacks.get_font_dirs = []() { return std::vector<std::string>(); };
	callbacks.on_install_pkgs = [](const std::vector<std::string>&) { return false; };
	callbacks.add_breakpoint = [](u32) {};
	callbacks.display_sleep_control_supported = []() { return false; };
	callbacks.enable_display_sleep = [](bool) {};
	callbacks.check_microphone_permissions = []() {};
	callbacks.make_video_source = []() { return std::unique_ptr<video_source>(); };
	callbacks.enable_gamemode = [](bool) {};
	callbacks.get_database_config = [](const std::string&) { return std::string(); };

	Emu.SetCallbacks(std::move(callbacks));

	// Initialize VFS + per-user dirs the same way headless_application
	// does on native. Without this, BootGame later hits null vfs state
	// when it tries to resolve the boot path.
	Emu.Init();
	printf("[Rpcs3VmJs] Emu.Init() returned\n");
#endif
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
	std::thread([this, path, title_id, direct]() {
		printf("[Rpcs3VmJs] BootGame thread entered\n");
		Emu.GracefulShutdown(false);
		printf("[Rpcs3VmJs] Emu.GracefulShutdown(false) returned\n");

		printf("[Rpcs3VmJs] calling Emu.BootGame(path='%s', tid='%s', direct=%d)...\n",
			path.c_str(), title_id.c_str(), direct ? 1 : 0);
		const auto error = Emu.BootGame(path, title_id, direct);
		printf("[Rpcs3VmJs] Emu.BootGame returned: %d\n", static_cast<int>(error));
		if (error != game_boot_result::no_errors)
		{
			printf("[Rpcs3VmJs] Emu.BootGame failed: %d\n", static_cast<int>(error));
			m_status = Status::Stopped;
			return;
		}
		printf("[Rpcs3VmJs] Emu.BootGame succeeded — path=%s\n", Emu.GetBoot().c_str());
		printf("[Rpcs3VmJs] post-boot state: IsReady=%d IsStarting=%d IsRunning=%d IsStopped=%d\n",
			Emu.IsReady() ? 1 : 0, Emu.IsStarting() ? 1 : 0,
			Emu.IsRunning() ? 1 : 0, Emu.IsStopped() ? 1 : 0);
		if (Emu.IsReady())
		{
			printf("[Rpcs3VmJs] calling Emu.Run(true) to start PPU execution...\n");
			Emu.Run(true);
		}
		// BootGame's autostart path leaves us in 'starting' state. On
		// native rpcs3 the RSX thread spawns and calls RunPPU() via
		// CallFromMainThread; since we now wire NullGSRender as the
		// rsx::thread, that callback fires automatically. Don't call
		// RunPPU() here (would race with rsx::thread::on_task and can
		// double-clear stop flags before RSX is ready).
		printf("[Rpcs3VmJs] post-boot: deferring Run to rsx::thread::on_task\n");
		m_bootPath = Emu.GetBoot();
		m_status = Status::Running;
	}).detach();
#else
	// POC scaffold path — emucore not linked. Log the same milestones so
	// downstream code (Stats FPS pill, screenshot triggers, the WebGPU
	// render loop's `if (boot ready)` gate) sees the right state edges.
	printf("[Rpcs3VmJs] [POC] Emu.GracefulShutdown(false) [stub]\n");
	printf("[Rpcs3VmJs] [POC] Emu.BootGame('%s', '%s', %d) [stub]\n",
	       path.c_str(), title_id.c_str(), direct ? 1 : 0);
	m_bootPath = path;
	m_status = Status::Running;
#endif
}

void CRpcs3VmJs::BootElf(std::string path)
{
	printf("[Rpcs3VmJs] BootElf('%s')\n", path.c_str());
	Boot(path, "", /*direct=*/true);
}

void CRpcs3VmJs::BootDiscImage(std::string path)
{
	printf("[Rpcs3VmJs] BootDiscImage('%s')\n", path.c_str());

#ifdef HAS_EMUCORE
	g_booted_disc_path = path;
#endif

	// Phase 2 smoke test (plan-poc-ps3-webgpu.md) — exercise the JS disc
	// bridge before the emucore is hooked up, so we can confirm
	// end-to-end streaming works even in the standalone build.
	//
	// MUST run on a worker thread: BootDiscImage is invoked from the
	// embind entry point (main DOM thread). The bridge polls
	// `Module.discImageDevice.isDone()` in a tight loop while waiting
	// for File.slice() to resolve — and that .then() callback can only
	// fire when the main thread is idle. So calling the bridge inline
	// from main deadlocks. In the real boot path the same call lands on
	// the PPU thread (a pthread worker), which is fine — this detached
	// std::thread imitates that future caller.
	//
	// Reads:
	//   - bytes [0, 16)      — usually zero on ISO9660/UDF system areas
	//   - bytes [0x8000, 32) — ISO9660 Primary Volume Descriptor; offset
	//                          0x8001 carries the "CD001" magic for plain
	//                          ISO9660 disks. PS3 BD-ROMs are UDF + a
	//                          second ISO9660 layer so this should match
	//                          for most images served through the bridge.
	std::thread([]() {
		JsDiscBridge bridge;
		if (!bridge.HasFile())
		{
			printf("[Rpcs3VmJs] JsDiscBridge: no file attached on Module.discImageDevice\n");
			return;
		}
		const uint64_t totalSize = bridge.GetSize();
		printf("[Rpcs3VmJs] JsDiscBridge: file attached, size=%llu bytes\n",
		       static_cast<unsigned long long>(totalSize));

		uint8_t head[16] = {};
		const uint64_t got0 = bridge.ReadAt(0, head, sizeof(head));
		printf("[Rpcs3VmJs] JsDiscBridge: read %llu bytes @ 0  → %02X %02X %02X %02X %02X %02X %02X %02X\n",
		       static_cast<unsigned long long>(got0),
		       head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);

		if (totalSize >= 0x8000 + 16)
		{
			uint8_t pvd[16] = {};
			const uint64_t got1 = bridge.ReadAt(0x8000, pvd, sizeof(pvd));
			const bool magicOk = pvd[1] == 'C' && pvd[2] == 'D' && pvd[3] == '0' && pvd[4] == '0' && pvd[5] == '1';
			printf("[Rpcs3VmJs] JsDiscBridge: read %llu bytes @ 0x8000 → %02X '%c%c%c%c%c' (CD001 magic %s)\n",
			       static_cast<unsigned long long>(got1),
			       pvd[0],
			       pvd[1] ? pvd[1] : '.', pvd[2] ? pvd[2] : '.', pvd[3] ? pvd[3] : '.',
			       pvd[4] ? pvd[4] : '.', pvd[5] ? pvd[5] : '.',
			       magicOk ? "OK" : "miss");
		}
	}).detach();

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
