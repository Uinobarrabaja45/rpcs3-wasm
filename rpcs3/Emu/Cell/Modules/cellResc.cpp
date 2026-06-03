#include "stdafx.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/Cell/lv2/sys_semaphore.h"
#include "cellGcmSys.h"

#include "Emu/RSX/GCM.h"
#include "Emu/RSX/gcm_enums.h"
#include "cellResc.h"
#include "cellVideoOut.h"

LOG_CHANNEL(cellResc);

template <>
void fmt_class_string<CellRescError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](CellRescError value)
	{
		switch (value)
		{
		STR_CASE(CELL_RESC_ERROR_NOT_INITIALIZED);
		STR_CASE(CELL_RESC_ERROR_REINITIALIZED);
		STR_CASE(CELL_RESC_ERROR_BAD_ALIGNMENT);
		STR_CASE(CELL_RESC_ERROR_BAD_ARGUMENT);
		STR_CASE(CELL_RESC_ERROR_LESS_MEMORY);
		STR_CASE(CELL_RESC_ERROR_GCM_FLIP_QUE_FULL);
		STR_CASE(CELL_RESC_ERROR_BAD_COMBINATION);
		STR_CASE(CELL_RESC_ERROR_x308);
		}

		return unknown;
	});
}

error_code cellRescInit(vm::cptr<CellRescInitConfig> initConfig)
{
	cellResc.todo("cellRescInit(initConfig=*0x%x)", initConfig);

	auto& resc_manager = g_fxo->get<cell_resc_manager>();

	if (resc_manager.is_initialized)
	{
		return CELL_RESC_ERROR_REINITIALIZED;
	}

	if (!initConfig || initConfig->size > 28) // TODO: more checks
	{
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

	resc_manager.config =
	{
		initConfig->size,
		initConfig->resourcePolicy,
		initConfig->supportModes,
		initConfig->ratioMode,
		initConfig->palTemporalMode,
		initConfig->interlaceMode,
		initConfig->flipMode
	};
	resc_manager.is_initialized = true;

	return CELL_OK;
}

void cellRescExit()
{
	cellResc.todo("cellRescExit()");

	auto& resc_manager = g_fxo->get<cell_resc_manager>();
	resc_manager.is_initialized = false;
}

error_code cellRescVideoOutResolutionId2RescBufferMode(u32 resolutionId, vm::cptr<u32> bufferMode)
{
	cellResc.todo("cellRescVideoOutResolutionId2RescBufferMode(resolutionId=%d, bufferMode=*0x%x)", resolutionId, bufferMode);

	if (!bufferMode || !resolutionId || resolutionId > CELL_VIDEO_OUT_RESOLUTION_576)
	{
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

	return CELL_OK;
}

error_code cellRescSetDsts(u32 bufferMode, vm::cptr<CellRescDsts> dsts)
{
	cellResc.todo("cellRescSetDsts(bufferMode=%d, dsts=*0x%x)", bufferMode, dsts);

	auto& resc_manager = g_fxo->get<cell_resc_manager>();

	if (!resc_manager.is_initialized)
	{
		return CELL_RESC_ERROR_NOT_INITIALIZED;
	}

	if (!dsts || !bufferMode || bufferMode > CELL_RESC_1920x1080) // TODO: is the bufferMode check correct?
	{
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

	return CELL_OK;
}

error_code cellRescSetDisplayMode(u32 bufferMode)
{
	cellResc.todo("cellRescSetDisplayMode(bufferMode=%d)", bufferMode);

	auto& resc_manager = g_fxo->get<cell_resc_manager>();
#ifdef __EMSCRIPTEN__
	printf("[RESC] SetDisplayMode bufferMode=%u support=0x%x init=%d max1080=%u\n", bufferMode, static_cast<unsigned>(resc_manager.config.support_modes), static_cast<int>(resc_manager.is_initialized), static_cast<unsigned>(CELL_RESC_1920x1080));
#endif

	if (!resc_manager.is_initialized)
	{
		return CELL_RESC_ERROR_NOT_INITIALIZED;
	}

#ifdef __EMSCRIPTEN__
	// wasm: Terraria reaches resc setup with an incomplete config during the
	// (still-stalling) boot, so bufferMode arrives 0/unsupported -> BAD_ARGUMENT.
	// That breaks the display-buffer chain and leaves the WebGPU blitter with no
	// framebuffer (fb=0x0, flip=0). Coerce an invalid mode to a sane default
	// (1280x720) and make the support mask cover it so ConvertAndFlip can proceed.
	if (!bufferMode || bufferMode > CELL_RESC_1920x1080)
	{
		printf("[RESC] SetDisplayMode coerce bufferMode %u -> 1280x720 (wasm)\n", static_cast<unsigned>(bufferMode));
		bufferMode = CELL_RESC_1280x720;
	}
	resc_manager.config.support_modes |= bufferMode;
#endif

	if (!bufferMode || bufferMode > CELL_RESC_1920x1080 || !(resc_manager.config.support_modes & bufferMode)) // TODO: is the bufferMode check correct?
	{
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

	if (bufferMode == CELL_RESC_720x576)
	{
		const u32 pal_mode  = resc_manager.config.pal_temporal_mode;
		const u32 flip_mode = resc_manager.config.flip_mode;

		// Check if palTemporalMode is any INTERPOLATE mode or CELL_RESC_PAL_60_DROP
		if ((pal_mode - CELL_RESC_PAL_60_INTERPOLATE) <= CELL_RESC_PAL_60_INTERPOLATE || pal_mode == CELL_RESC_PAL_60_DROP)
		{
			if (flip_mode == CELL_RESC_DISPLAY_HSYNC)
			{
				return CELL_RESC_ERROR_BAD_COMBINATION;
			}
		}

		if (pal_mode == CELL_RESC_PAL_60_FOR_HSYNC)
		{
			if (flip_mode == CELL_RESC_DISPLAY_VSYNC)
			{
				return CELL_RESC_ERROR_BAD_COMBINATION;
			}
		}
	}

	resc_manager.buffer_mode = bufferMode;

	return CELL_OK;
}

error_code cellRescAdjustAspectRatio(f32 horizontal, f32 vertical)
{
	cellResc.todo("cellRescAdjustAspectRatio(horizontal=%f, vertical=%f)", horizontal, vertical);

	auto& resc_manager = g_fxo->get<cell_resc_manager>();

	if (!resc_manager.is_initialized)
	{
		return CELL_RESC_ERROR_NOT_INITIALIZED;
	}

	if (horizontal < 0.5f || horizontal > 2.0f || vertical < 0.5f || vertical > 2.0f)
	{
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

	return CELL_OK;
}

error_code cellRescSetPalInterpolateDropFlexRatio(f32 ratio)
{
	cellResc.todo("cellRescSetPalInterpolateDropFlexRatio(ratio=%f)", ratio);

	auto& resc_manager = g_fxo->get<cell_resc_manager>();

	if (!resc_manager.is_initialized)
	{
		return CELL_RESC_ERROR_NOT_INITIALIZED;
	}

	if (ratio < 0.0f || ratio > 1.0f)
	{
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

	return CELL_OK;
}

error_code cellRescGetBufferSize(vm::ptr<s32> colorBuffers, vm::ptr<s32> vertexArray, vm::ptr<s32> fragmentShader)
{
	cellResc.todo("cellRescGetBufferSize(colorBuffers=*0x%x, vertexArray=*0x%x, fragmentShader=*0x%x)", colorBuffers, vertexArray, fragmentShader);

	auto& resc_manager = g_fxo->get<cell_resc_manager>();

	if (!resc_manager.is_initialized)
	{
		return CELL_RESC_ERROR_NOT_INITIALIZED;
	}

	//if (something)
	//{
	//	return CELL_RESC_ERROR_x308;
	//}

	//if (colorBuffers)
	//{
	//	colorBuffers = something
	//}

	//if (vertexArray)
	//{
	//	vertexArray = something
	//}

	//if (fragmentShader)
	//{
	//	fragmentShader = something
	//}

	return CELL_OK;
}

s32 cellRescGetNumColorBuffers(u32 dstMode, u32 palTemporalMode, s32 reserved)
{
	cellResc.todo("cellRescGetNumColorBuffers(dstMode=%d, palTemporalMode=%d, reserved=%d)", dstMode, palTemporalMode, reserved);

	if (reserved != 0)
	{
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

	if (dstMode == CELL_RESC_720x576)
	{
		// Check if palTemporalMode is any INTERPOLATE mode
		if ((palTemporalMode - CELL_RESC_PAL_60_INTERPOLATE) <= CELL_RESC_PAL_60_INTERPOLATE)
		{
			return 6;
		}

		if (palTemporalMode == CELL_RESC_PAL_60_DROP)
		{
			return 3;
		}
	}

	return 2;
}

error_code cellRescGcmSurface2RescSrc(vm::cptr<CellGcmSurface> gcmSurface, vm::ptr<CellRescSrc> rescSrc)
{
	cellResc.todo("cellRescGcmSurface2RescSrc(gcmSurface=*0x%x, rescSrc=*0x%x)", gcmSurface, rescSrc);

	if (!gcmSurface || !rescSrc)
	{
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

	// Real libresc converts the GCM color-surface descriptor into a RESC source
	// descriptor. The old stub returned CELL_OK WITHOUT writing rescSrc, so the
	// caller's rescSrc stayed zero-filled and the following cellRescSetSrc rejected
	// it (width/height == 0 -> CELL_RESC_ERROR_BAD_ARGUMENT). That broke the entire
	// present path: no display buffer was ever published to WebGPUGSRender::flip(),
	// so the canvas never showed a real frame. Map color-buffer A (index 0) -> src.
	u32 bytes_per_pixel;
	u32 resc_format;
	switch (gcmSurface->colorFormat)
	{
	case CELL_GCM_SURFACE_F_W16Z16Y16X16:
		resc_format = CELL_RESC_SURFACE_F_W16Z16Y16X16;
		bytes_per_pixel = 8;
		break;
	case CELL_GCM_SURFACE_A8R8G8B8:
	default:
		resc_format = CELL_RESC_SURFACE_A8R8G8B8;
		bytes_per_pixel = 4;
		break;
	}

	const u16 w = gcmSurface->width;
	const u16 h = gcmSurface->height;
	const u32 pitch = gcmSurface->colorPitch[0] ? +gcmSurface->colorPitch[0] : u32{w} * bytes_per_pixel;

	rescSrc->format = resc_format;
	rescSrc->pitch  = pitch;
	rescSrc->width  = w;
	rescSrc->height = h;
	rescSrc->offset = gcmSurface->colorOffset[0];

#ifdef __EMSCRIPTEN__
	{ static thread_local u32 s_g2r = 0; if ((++s_g2r & 0xFFu) == 0u) printf("[RESC] GcmSurface2RescSrc colorFormat=%u %ux%u pitch=%u offset=0x%x loc=%u -> rescSrc fmt=%u (1/256)\n",
		static_cast<unsigned>(gcmSurface->colorFormat), static_cast<unsigned>(w), static_cast<unsigned>(h),
		static_cast<unsigned>(pitch), static_cast<unsigned>(+gcmSurface->colorOffset[0]),
		static_cast<unsigned>(gcmSurface->colorLocation[0]), static_cast<unsigned>(resc_format)); }
#endif

	return CELL_OK;
}

error_code cellRescSetSrc(s32 idx, vm::cptr<CellRescSrc> src)
{
	cellResc.todo("cellRescSetSrc(idx=0x%x, src=*0x%x)", idx, src);

	auto& resc_manager = g_fxo->get<cell_resc_manager>();

	if (!resc_manager.is_initialized)
	{
		return CELL_RESC_ERROR_NOT_INITIALIZED;
	}

	if (idx >= SRC_BUFFER_NUM || !src || !src->width || !src->height || src->height > 4096) // TODO: is this correct?
	{
#ifdef __EMSCRIPTEN__
		printf("[RESC] cellRescSetSrc REJECT idx=%d src=0x%x w=%u h=%u pitch=%u off=0x%x\n",
			idx, src.addr(),
			src ? static_cast<unsigned>(src->width) : 0u, src ? static_cast<unsigned>(src->height) : 0u,
			src ? static_cast<unsigned>(src->pitch) : 0u, src ? static_cast<unsigned>(src->offset) : 0u);
#endif
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

	const auto render = rsx::get_current_renderer();
	auto& gcm_cfg = g_fxo->get<gcm_config>();
	
	render->display_buffers[idx].offset = src->offset;
	render->display_buffers[idx].pitch = src->pitch;
	render->display_buffers[idx].width = src->width;
	render->display_buffers[idx].height = src->height;

	gcm_cfg.gcm_buffers[idx].offset = src->offset;
	gcm_cfg.gcm_buffers[idx].pitch = src->pitch;
	gcm_cfg.gcm_buffers[idx].width = src->width;
	gcm_cfg.gcm_buffers[idx].height = src->height;

	if (idx + 1u > render->display_buffers_count)
	{
		render->display_buffers_count = idx + 1;
	}

#ifdef __EMSCRIPTEN__
	{ static thread_local u32 s_ss = 0; if ((++s_ss & 0xFFu) == 0u) printf("[RESC] cellRescSetSrc idx=%d offset=0x%x pitch=%u %ux%u count=%u (1/256)\n",
		idx, static_cast<unsigned>(src->offset), static_cast<unsigned>(src->pitch),
		static_cast<unsigned>(src->width), static_cast<unsigned>(src->height),
		render->display_buffers_count); }
#endif

	return CELL_OK;
}

error_code cellRescSetConvertAndFlip(ppu_thread& ppu, vm::ptr<CellGcmContextData> con, s32 idx)
{
	cellResc.todo("cellRescSetConvertAndFlip(con=*0x%x, idx=0x%x)", con, idx);

	auto& resc_manager = g_fxo->get<cell_resc_manager>();

	if (!resc_manager.is_initialized)
	{
		return CELL_RESC_ERROR_NOT_INITIALIZED;
	}

	if (idx >= SRC_BUFFER_NUM)
	{
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

#ifdef __EMSCRIPTEN__
	{ static thread_local u32 s_caf = 0; if ((++s_caf & 0xFFu) == 0u) printf("[RESC] cellRescSetConvertAndFlip idx=%d (1/256)\n", idx); }
	// FRAMESTACK (2026-06-03): the game is HARD-GATED — it runs this present every frame but never
	// issues a real scene draw (presents black). Capture main's frame-loop CALL STACK here (once) so we
	// can disassemble the frame routine and find the conditional that SKIPS the splash draw. PPC64 ELFv1
	// back-chain: back ptr @[sp+4] low word, saved LR @[sp+20] low word (BE 8B slots).
	{
		static thread_local bool s_fs = false;
		if (!s_fs && ppu.id == 0x1000003u)   // RenderLoop is the present caller (LR=0x3f7ae4), not main
		{
			s_fs = true;
			printf("[FRAMESTACK] RenderLoop present cia=0x%x lr=0x%x r1=0x%x idx=%d con=0x%x\n",
				static_cast<u32>(ppu.cia), static_cast<u32>(ppu.lr), static_cast<u32>(ppu.gpr[1]), idx, con.addr());
			u32 sp = static_cast<u32>(ppu.gpr[1]);
			for (u32 f = 0; f < 16 && sp >= 0x10000u && sp < 0x60000000u; f++)
			{
				const u32 back = vm::read32(sp + 4);
				const u32 slr  = vm::read32(sp + 20);
				printf("  [FRAMESTACK-FR] #%u sp=0x%x savedLR=0x%x back=0x%x\n", f, sp, slr, back);
				if (back <= sp) break;
				sp = back;
			}
		}
	}
#endif

	if (con->current + 2 >= con->end)
	{
		if (s32 res = con->callback(ppu, con, 8))
		{
			cellResc.error("cellRescSetConvertAndFlip: callback failed (0x%08x)", res);
			return not_an_error(res);
		}
	}

	const u32 cmd_size = 8;
	*con->current++ = GCM_FLIP_COMMAND << 2 | 1u << 18;
	*con->current++ = static_cast<u32>(idx);

	auto& gcm_cfg = g_fxo->get<gcm_config>();
	if (con.addr() == gcm_cfg.gcm_info.context_addr)
	{
		vm::_ptr<CellGcmControl>(gcm_cfg.gcm_info.control_addr)->put += cmd_size;
	}

#ifdef __EMSCRIPTEN__
	// FLIP-COMPLETE signalling. Both main_thread (blocks on sem 0x96004600) AND the EE RenderLoop
	// (busy-polls a flip-done flag right after cellRescSetWaitFlip) wait for the flip to complete.
	// On real HW the game's RESC flip handler — registered via cellRescSetFlipHandler — does that
	// signalling when the flip interrupt fires. Here the flip is injected as a raw GCM_FLIP_COMMAND
	// above, bypassing the flip-notification path, so the handler never runs and the frame loop
	// stalls one frame in. Invoke the registered handler directly on this (RenderLoop) PPU thread so
	// it performs its real work (posts main's idx6 sem AND sets the RenderLoop's flip-done flag),
	// letting the whole frame loop sustain. Also hard-post idx6 as a belt-and-braces fallback.
	if (const auto render = rsx::get_current_renderer(); render && render->flip_handler)
	{
		render->flip_handler(ppu, 0);
	}
	sys_semaphore_post(ppu, 0x96004600u, 1);
#endif

	return CELL_OK;
}

error_code cellRescSetWaitFlip(vm::ptr<CellGcmContextData> con)
{
	cellResc.todo("cellRescSetWaitFlip(con=*0x%x)", con);

	return CELL_OK;
}

error_code cellRescSetBufferAddress(vm::cptr<u32> colorBuffers, vm::cptr<u32> vertexArray, vm::cptr<u32> fragmentShader)
{
	cellResc.todo("cellRescSetBufferAddress(colorBuffers=*0x%x, vertexArray=*0x%x, fragmentShader=*0x%x)", colorBuffers, vertexArray, fragmentShader);

	auto& resc_manager = g_fxo->get<cell_resc_manager>();

	if (!resc_manager.is_initialized)
	{
		return CELL_RESC_ERROR_NOT_INITIALIZED;
	}

	if (!colorBuffers || !vertexArray || !fragmentShader)
	{
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

	if (!colorBuffers.aligned(128) || !vertexArray.aligned(4) || !fragmentShader.aligned(64)) // clrlwi with 25, 30, 26
	{
		return CELL_RESC_ERROR_BAD_ALIGNMENT;
	}

	return CELL_OK;
}

void cellRescSetFlipHandler(vm::ptr<void(u32)> handler)
{
	cellResc.todo("cellRescSetFlipHandler(handler=*0x%x)", handler);
	if (const auto rsx = rsx::get_current_renderer(); rsx->is_initialized)
	{
		rsx->flip_handler = handler;
	}
}

void cellRescResetFlipStatus()
{
	cellResc.todo("cellRescResetFlipStatus()");
	if (const auto rsx = rsx::get_current_renderer(); rsx->is_initialized)
	{
		rsx->flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_WAITING;
	}
}

s32 cellRescGetFlipStatus()
{
	cellResc.todo("cellRescGetFlipStatus()");
	return rsx::get_current_renderer()->flip_status;
}

s32 cellRescGetRegisterCount()
{
	cellResc.todo("cellRescGetRegisterCount()");
	return 0;
}

u64 cellRescGetLastFlipTime()
{
	cellResc.todo("cellRescGetLastFlipTime()");
	return rsx::get_current_renderer()->last_guest_flip_timestamp;
}

void cellRescSetRegisterCount(s32 regCount)
{
	cellResc.todo("cellRescSetRegisterCount(regCount=0x%x)", regCount);
}

void cellRescSetVBlankHandler(vm::ptr<void(u32)> handler)
{
	cellResc.todo("cellRescSetVBlankHandler(handler=*0x%x)", handler);
	if (const auto rsx = rsx::get_current_renderer(); rsx->is_initialized)
	{
		rsx->vblank_handler = handler;
	}
}

error_code cellRescCreateInterlaceTable(vm::ptr<void> ea_addr, f32 srcH, CellRescTableElement depth, s32 length)
{
	cellResc.todo("cellRescCreateInterlaceTable(ea_addr=0x%x, srcH=%f, depth=%d, length=%d)", ea_addr, srcH, +depth, length);

	if (!ea_addr || srcH <= 0.0f || depth > CELL_RESC_ELEMENT_FLOAT || length <= 0) // TODO: srcH check correct?
	{
		return CELL_RESC_ERROR_BAD_ARGUMENT;
	}

	auto& resc_manager = g_fxo->get<cell_resc_manager>();

	if (!resc_manager.buffer_mode)
	{
		return CELL_RESC_ERROR_BAD_COMBINATION;
	}

	return CELL_OK;
}


DECLARE(ppu_module_manager::cellResc)("cellResc", []()
{
	REG_FUNC(cellResc, cellRescSetConvertAndFlip);
	REG_FUNC(cellResc, cellRescSetWaitFlip);
	REG_FUNC(cellResc, cellRescSetFlipHandler);
	REG_FUNC(cellResc, cellRescGcmSurface2RescSrc);
	REG_FUNC(cellResc, cellRescGetNumColorBuffers);
	REG_FUNC(cellResc, cellRescSetDsts);
	REG_FUNC(cellResc, cellRescResetFlipStatus);
	REG_FUNC(cellResc, cellRescSetPalInterpolateDropFlexRatio);
	REG_FUNC(cellResc, cellRescGetRegisterCount);
	REG_FUNC(cellResc, cellRescAdjustAspectRatio);
	REG_FUNC(cellResc, cellRescSetDisplayMode);
	REG_FUNC(cellResc, cellRescExit);
	REG_FUNC(cellResc, cellRescInit);
	REG_FUNC(cellResc, cellRescGetBufferSize);
	REG_FUNC(cellResc, cellRescGetLastFlipTime);
	REG_FUNC(cellResc, cellRescSetSrc);
	REG_FUNC(cellResc, cellRescSetRegisterCount);
	REG_FUNC(cellResc, cellRescSetBufferAddress);
	REG_FUNC(cellResc, cellRescGetFlipStatus);
	REG_FUNC(cellResc, cellRescVideoOutResolutionId2RescBufferMode);
	REG_FUNC(cellResc, cellRescSetVBlankHandler);
	REG_FUNC(cellResc, cellRescCreateInterlaceTable);
});
