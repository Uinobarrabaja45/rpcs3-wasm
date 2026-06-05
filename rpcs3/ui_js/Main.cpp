// RPCS3 Emscripten / WebGPU entry point — POC scaffold.
//
// This file is the WASM-side equivalent of headless_application.cpp /
// main.cpp combined: it sets up the emulator core, wires it to a WebGPU
// surface backed by the browser's GPUDevice, registers the keyboard
// callback, and exposes a small embind surface so the React UI can drive
// it (initVmWebGPU, bootElf, bootDiscImage, setResolution, getFrames…).
//
// The shape mirrors Play-GamePlaytoo/Source/ui_js/Main.cpp — when the
// rpcs3 emucore can be cross-compiled with emcc, the printf stubs in
// Rpcs3VmJs.cpp should be replaced with real Emu.* calls.

#include <cstdio>
#include <cstdint>
#include <atomic>
#include <string>

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <emscripten/html5.h>

#include "Rpcs3VmJs.h"
#include "InputProviderEmscripten.h"
#ifdef HAS_WEBGPU
#include "GSH_WebGPUJs.h"
#include <webgpu/webgpu.h>
#include <webgpu/webgpu_cpp.h>
#endif

// Globals — see Play!'s Main.cpp for the analogous singletons. They live
// at file scope because they're written from the embind entry points
// (which can be called from any thread) and read by the keyboard callback
// (which runs on the main DOM thread).
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_context = 0;
static std::shared_ptr<CInputProviderEmscripten> g_inputProvider;
#ifdef HAS_WEBGPU
static CGSH_WebGPUJs* g_gsHandler = nullptr;
// The game's RSX flip counter (defined in WebGPUGSRender.cpp). Bumped ONLY when
// the game presents a real frame — unlike g_frameCount, which also counts the
// ~2/s boot keep-alive presents. Exposed to JS as getFlipCount() so the loading
// gauge can tell "still booting" (flip == 0) from "game is rendering" (flip > 0).
extern std::atomic<std::uint64_t> g_webgpu_flip_count;
// Count of VARIED (real on-screen content) composites — stays 0 through the
// all-black/navy boot, non-zero once the game is actually VISIBLE. This is the
// signal the loading gauge uses to hide (flip_count alone fires on boot's black
// flips, before anything is visible).
extern std::atomic<std::uint32_t> g_webgpu_content_frames;
#endif

// ───────── stats / pacing knobs exposed to JS ─────────
// Bumped from the frame loop (real backend would update these from the
// RSX present callback). Read from JS via getFrames() / clearStats().
static int g_frameCount = 0;
static int g_renderWidth = 1280;
static int g_renderHeight = 720;
static int g_minDrawIntervalMs = 0;
static int g_frameskip = 0;
static int g_maxTextureUploadsPerFrame = 0;
static long g_lastSleepMicroseconds = 0;

#ifdef HAS_WEBGPU
// Browser main-loop tick. Hooked via emscripten_set_main_loop after
// initVmWebGPU completes; runs at requestAnimationFrame rate (~60 Hz on
// most displays, throttled to 30 Hz on backgrounded tabs). Each tick:
//   1. If the VM has booted, mark the GS handler as 'booted' so it
//      switches from splash colors to running colors.
//   2. Call GSH render — clears the surface, builds a command buffer,
//      submits. emdawnwebgpu auto-presents when the event loop yields.
//   3. Bump g_frameCount so the JS-side Stats pill shows a non-zero FPS.
//
// Once the RSX backend lands (Phase 4), step 2 becomes "replay RSX
// commands captured since last vsync"; the loop shape stays identical.
static void mainLoopTick()
{
	// PPU/SPU pthread workers abort under wasm32 (unaligned access in
	// guest-code emulation, plan-poc-ps3-webgpu.md Phase 4). Their abort
	// can taint our main-thread wgpu state — keep this loop alive
	// regardless so the React canvas stays visible and the boot trigger
	// pipeline keeps logging.
	if (!g_gsHandler) return;
	bool booted = false;
	if (g_virtualMachine)
	{
		booted = g_virtualMachine->IsRunning();
	}
	g_gsHandler->SetBooted(booted);
	if (g_gsHandler->Render())
	{
		g_frameCount++;
	}
}
#endif

extern "C" {

int main(int argc, const char** argv)
{
	printf("RPCS3 (WebGPU POC) - main()\n");
	return 0;
}

// Browser → emulator keyboard bridge.
static EM_BOOL keyboardCallback(int eventType, const EmscriptenKeyboardEvent* keyEvent, void* /*userData*/)
{
	if (keyEvent->repeat) return EM_TRUE;
	switch (eventType)
	{
	case EMSCRIPTEN_EVENT_KEYDOWN:
		if (g_inputProvider) g_inputProvider->OnKeyDown(keyEvent->code);
		break;
	case EMSCRIPTEN_EVENT_KEYUP:
		if (g_inputProvider) g_inputProvider->OnKeyUp(keyEvent->code);
		break;
	}
	return EM_TRUE;
}

// Headless / no-WebGPU init. Useful for testing the PPU/SPU pipeline in
// isolation without a render backend. The JS layer falls back to this when
// navigator.gpu is missing or ?webgpu=false.
void initVm()
{
	printf("[Main] initVm (headless)\n");

	if (!g_virtualMachine)
	{
		g_virtualMachine = new CRpcs3VmJs();
		g_virtualMachine->CreateVM();
	}
	g_inputProvider = std::make_shared<CInputProviderEmscripten>();

	emscripten_set_keydown_callback("#outputCanvas", nullptr, EM_FALSE, &keyboardCallback);
	emscripten_set_keyup_callback("#outputCanvas", nullptr, EM_FALSE, &keyboardCallback);
}

} // extern "C"

#ifdef HAS_WEBGPU
// Pull the WGPUDevice out of the JS GPUDevice that the page handed us. The
// emdawnwebgpu port exposes Module.WebGPU.importJsDevice (or .mgr.importJs…
// in older builds); we try each location in turn. This is the same dance
// Play!'s Main.cpp does — see js/play_browser/src/Rpcs3Module.ts for the JS
// side.
EM_JS(WGPUDevice, getDeviceHandle, (emscripten::EM_VAL device_val, emscripten::EM_VAL adapter_val), {
	var valObj = typeof Emval !== 'undefined' ? Emval : (typeof EmscriptenVal !== 'undefined' ? EmscriptenVal : null);
	if (!valObj) { console.error('getDeviceHandle: Emval missing'); return 0; }
	var device = valObj.toValue(device_val);
	var adapter = adapter_val ? valObj.toValue(adapter_val) : null;
	if (!device) { console.error('getDeviceHandle: device val null'); return 0; }

	var webgpu = Module['WebGPU'];
	if (!webgpu) { console.error('getDeviceHandle: Module.WebGPU missing'); return 0; }

	if (adapter && typeof webgpu['importJsAdapter'] === 'function') {
		try { webgpu['importJsAdapter'](adapter); } catch (e) { console.warn(e); }
	}
	var importers = [
		webgpu['importJsDevice'],
		webgpu['mgr'] && webgpu['mgr']['importJsDevice'],
		webgpu['Internals'] && webgpu['Internals']['importJsDevice'],
	];
	for (var i = 0; i < importers.length; i++) {
		var fn = importers[i];
		if (typeof fn === 'function') {
			try {
				var h = fn.call(webgpu, device);
				if (h) return h;
			} catch (e) { console.warn('importJsDevice variant failed:', e); }
		}
	}
	console.error('getDeviceHandle: no importJsDevice found');
	return 0;
});

EM_JS(int, canCanvasBeAccessed, (emscripten::EM_VAL canvas_val), {
	try {
		var valObj = typeof Emval !== 'undefined' ? Emval : (typeof EmscriptenVal !== 'undefined' ? EmscriptenVal : null);
		var canvas = (canvas_val && valObj) ? valObj.toValue(canvas_val) : Module.canvas;
		return canvas ? 1 : 0;
	} catch (e) { return 0; }
});

extern "C" void initVmWebGPU(emscripten::val device, emscripten::val adapter, emscripten::val canvas, std::string backend)
{
	printf("[Main] initVmWebGPU backend='%s'\n", backend.c_str());

	if (!canCanvasBeAccessed(canvas.as_handle()))
	{
		printf("ERROR: Canvas not accessible from this thread\n");
		emscripten_throw_string("Canvas not accessible — WebGPU init aborted");
		return;
	}

	try
	{
		WGPUDevice deviceHandle = getDeviceHandle(device.as_handle(), adapter.as_handle());
		if (!deviceHandle)
		{
			printf("ERROR: getDeviceHandle returned 0\n");
			throw std::runtime_error("Failed to import GPUDevice");
		}

		wgpu::InstanceDescriptor instanceDesc = {};
		wgpu::Instance instance = wgpu::CreateInstance(&instanceDesc);
		if (!instance) throw std::runtime_error("Failed to create WebGPU instance");

		wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc = {};
		canvasDesc.selector = "#outputCanvas";
		wgpu::SurfaceDescriptor surfaceDesc = {};
		surfaceDesc.nextInChain = &canvasDesc;
		wgpu::Surface surface = instance.CreateSurface(&surfaceDesc);
		if (!surface) throw std::runtime_error("Failed to create WebGPU surface");

		// Acquire ownership ONCE — see Play!'s Main.cpp comment about
		// double-Acquire freeing the device prematurely.
		wgpu::Device ownedDevice = wgpu::Device::Acquire(deviceHandle);

		// Pre-configure the surface on the main thread; emdawnwebgpu's
		// Configure() reads Module.canvas which is undefined on workers.
		{
			wgpu::SurfaceConfiguration surfConfig = {};
			surfConfig.device      = ownedDevice;
			surfConfig.format      = wgpu::TextureFormat::BGRA8Unorm;
			surfConfig.usage       = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopyDst;
			surfConfig.alphaMode   = wgpu::CompositeAlphaMode::Auto;
			surfConfig.width       = static_cast<uint32_t>(g_renderWidth);
			surfConfig.height      = static_cast<uint32_t>(g_renderHeight);
			surfConfig.presentMode = wgpu::PresentMode::Fifo;
			surface.Configure(&surfConfig);
		}

		if (!g_virtualMachine)
		{
			g_virtualMachine = new CRpcs3VmJs();
			g_virtualMachine->CreateVM();
		}

		g_gsHandler = new CGSH_WebGPUJs(ownedDevice, instance, surface, backend);
		g_gsHandler->SetPresentationParams(g_renderWidth, g_renderHeight);
		g_gsHandler->Initialize();

		g_inputProvider = std::make_shared<CInputProviderEmscripten>();
		emscripten_set_keydown_callback("#outputCanvas", nullptr, EM_FALSE, &keyboardCallback);
		emscripten_set_keyup_callback("#outputCanvas", nullptr, EM_FALSE, &keyboardCallback);

		// Kick off the render loop. fps=0 + simulateInfiniteLoop=0 lets
		// the browser pick the rate (vsync-aligned). Passing fps>0 forces
		// setInterval which jitters worse on Safari; 0 is the right pick.
		// simulateInfiniteLoop=0 returns control to JS so the embind call
		// returns normally (we're not running argc/argv mode).
		emscripten_set_main_loop(&mainLoopTick, /*fps=*/0, /*simulateInfiniteLoop=*/0);

		printf("[Main] initVmWebGPU completed — main loop armed\n");
	}
	catch (const std::exception& e)
	{
		printf("ERROR initVmWebGPU: %s\n", e.what());
		emscripten_throw_string(e.what());
	}
}

// JS-driven device replacement after a device.lost event. The React shell
// requests a fresh GPUDevice and hands it here; we tear down the GS
// handler, rebuild the surface, and resume.
extern "C" void recreateWebGPUDevice(emscripten::val device, emscripten::val adapter)
{
	printf("[Main] recreateWebGPUDevice\n");
	if (g_gsHandler)
	{
		delete g_gsHandler;
		g_gsHandler = nullptr;
	}
	initVmWebGPU(device, adapter, emscripten::val::global("document").call<emscripten::val>("getElementById", std::string("outputCanvas")), std::string("recovery"));
}
#endif // HAS_WEBGPU

// ───────── small embind surface used by the React UI ─────────
namespace
{
	void bootElf(std::string path)
	{
		if (g_virtualMachine) g_virtualMachine->BootElf(std::move(path));
	}
	void bootDiscImage(std::string path)
	{
		if (g_virtualMachine) g_virtualMachine->BootDiscImage(std::move(path));
	}
	void bootXmb()
	{
		if (g_virtualMachine) g_virtualMachine->BootXmb();
	}
	void pause()       { if (g_virtualMachine) g_virtualMachine->Pause();  }
	void resume()      { if (g_virtualMachine) g_virtualMachine->Resume(); }
	void stop()        { if (g_virtualMachine) g_virtualMachine->Stop();   }
	int  getFrames()   { return g_frameCount; }
	void clearStats()  { g_frameCount = 0; }
	// Game-render signal for the JS loading gauge: the RSX flip count stays 0 all
	// through boot and goes non-zero only once the game presents its first real
	// frame (getFrames can't be used for this — it also counts boot keep-alives).
	int  getFlipCount()
	{
#ifdef HAS_WEBGPU
		return static_cast<int>(g_webgpu_flip_count.load(std::memory_order_relaxed));
#else
		return 0;
#endif
	}
	// "Is the game actually VISIBLE yet?" — 0 all through boot (black/navy frames),
	// non-zero once a real varied frame is composited. The loading gauge hides on this.
	int  getContentFrames()
	{
#ifdef HAS_WEBGPU
		return static_cast<int>(g_webgpu_content_frames.load(std::memory_order_relaxed));
#else
		return 0;
#endif
	}
	// Compile time of this wasm — shown in the UI so the user can tell at a glance
	// whether their browser loaded the freshly-built (flash-fixed) wasm or a stale one.
	std::string getBuildTag() { return std::string(__DATE__ " " __TIME__); }
	void setResolution(int w, int h)
	{
		g_renderWidth = w;
		g_renderHeight = h;
#ifdef HAS_WEBGPU
		if (g_gsHandler) { g_gsHandler->SetPresentationParams(w, h); g_gsHandler->ConfigureSurface(); }
#endif
	}
	void setFrameskip(int n) { g_frameskip = n; }
	void setMinDrawIntervalMs(int n) { g_minDrawIntervalMs = n; }
	void setMaxTextureUploadsPerFrame(int n) { g_maxTextureUploadsPerFrame = n; }
	long getLastSleepMicroseconds() { return g_lastSleepMicroseconds; }
}

EMSCRIPTEN_BINDINGS(rpcs3_js_module)
{
	emscripten::function("bootElf",                 &bootElf);
	emscripten::function("bootDiscImage",           &bootDiscImage);
	emscripten::function("bootXmb",                 &bootXmb);
	emscripten::function("pause",                   &pause);
	emscripten::function("resume",                  &resume);
	emscripten::function("stop",                    &stop);
	emscripten::function("getFrames",               &getFrames);
	emscripten::function("getFlipCount",            &getFlipCount);
	emscripten::function("getContentFrames",        &getContentFrames);
	emscripten::function("getBuildTag",             &getBuildTag);
	emscripten::function("clearStats",              &clearStats);
	emscripten::function("setResolution",           &setResolution);
	emscripten::function("setFrameskip",            &setFrameskip);
	emscripten::function("setMinDrawIntervalMs",    &setMinDrawIntervalMs);
	emscripten::function("setMaxTextureUploadsPerFrame", &setMaxTextureUploadsPerFrame);
	emscripten::function("getLastSleepMicroseconds", &getLastSleepMicroseconds);
#ifdef HAS_WEBGPU
	emscripten::function("initVmWebGPU",            &initVmWebGPU);
	emscripten::function("recreateWebGPUDevice",    &recreateWebGPUDevice);
#endif
	emscripten::function("initVm",                  &initVm);
}
