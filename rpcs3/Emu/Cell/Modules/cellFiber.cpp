#include "stdafx.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/IdManager.h"

#include "cellFiber.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/PPUFunction.h"
#include "Emu/Cell/Modules/sysPrxForUser.h"
#include "Emu/Cell/lv2/sys_ppu_thread.h"
#include <mutex>
#include <unordered_map>
#include <vector>
#include <memory>
#include <thread>
#include <chrono>

LOG_CHANNEL(cellFiber);

// ── Minimal cellFiberPpuUtilWorkerControl implementation (2026-06-04) ─────────────────────────────────────────
// rpcs3's cellFiber is fully stubbed (every function just returns CELL_OK), so fiber/coroutine-based games never
// run their game logic. Dragon's Crown: RunFibers was a no-op → its FiberFunc worker threads exited → NO game
// fiber ever executed → no render, main polls forever. This implements the worker-control TASK model DC uses (it
// never calls cellFiberPpuYield, and signals each fiber ~143×): CreateFiber registers a fiber {entry, arg, stack};
// SendSignal marks it runnable; RunFibers (the worker-thread loop) runs each runnable fiber's entry to completion
// as a guest call on the fiber's own stack, re-runnable on the next signal. Process-static state (POC; move to
// g_fxo for production). The mutex is released before each guest call so a fiber may re-enter cellFiber (SendSignal).
namespace
{
	struct ppu_fiber_rec
	{
		vm::ptr<CellFiberPpuEntry> entry{};
		u64 arg = 0;
		u32 control_addr = 0;
		u32 stack_ea = 0;
		u32 stack_size = 0;
		atomic_t<bool> runnable{false};
		atomic_t<bool> running{false};
		atomic_t<u32>  signals{0}; // pending SendSignal count; WaitSignal blocks until > 0 then consumes one
		atomic_t<u32>  tid{0};     // guest ppu_thread id running this fiber (set after spawn, for WaitSignal)
	};

	struct ppu_worker_ctl
	{
		std::vector<u32> fiber_addrs;
		atomic_t<bool> shutdown{false};
	};

	std::mutex g_cf_mtx;
	std::unordered_map<u32, std::shared_ptr<ppu_fiber_rec>> g_cf_fibers; // CellFiberPpu addr -> rec
	std::unordered_map<u32, std::shared_ptr<ppu_worker_ctl>> g_cf_ctls;  // control addr -> worker ctl
}

template <>
void fmt_class_string<CellFiberError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](CellFiberError value)
	{
		switch (value)
		{
			STR_CASE(CELL_FIBER_ERROR_AGAIN);
			STR_CASE(CELL_FIBER_ERROR_INVAL);
			STR_CASE(CELL_FIBER_ERROR_NOMEM);
			STR_CASE(CELL_FIBER_ERROR_DEADLK);
			STR_CASE(CELL_FIBER_ERROR_PERM);
			STR_CASE(CELL_FIBER_ERROR_BUSY);
			STR_CASE(CELL_FIBER_ERROR_ABORT);
			STR_CASE(CELL_FIBER_ERROR_STAT);
			STR_CASE(CELL_FIBER_ERROR_ALIGN);
			STR_CASE(CELL_FIBER_ERROR_NULL_POINTER);
			STR_CASE(CELL_FIBER_ERROR_NOSYSINIT);
		}

		return unknown;
	});
}

error_code _cellFiberPpuInitialize()
{
	cellFiber.todo("_cellFiberPpuInitialize()");

	return CELL_OK;
}

error_code _cellFiberPpuSchedulerAttributeInitialize(vm::ptr<CellFiberPpuSchedulerAttribute> attr, u32 sdkVersion)
{
	cellFiber.warning("_cellFiberPpuSchedulerAttributeInitialize(attr=*0x%x, sdkVersion=0x%x)", attr, sdkVersion);

	if (!attr)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!attr.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	memset(attr.get_ptr(), 0, sizeof(CellFiberPpuSchedulerAttribute));
	attr->autoCheckFlags = false;
	attr->autoCheckFlagsIntervalUsec = 0;
	attr->debuggerSupport = false;

	return CELL_OK;
}

error_code cellFiberPpuInitializeScheduler(vm::ptr<CellFiberPpuScheduler> scheduler, vm::ptr<CellFiberPpuSchedulerAttribute> attr)
{
	cellFiber.todo("cellFiberPpuInitializeScheduler(scheduler=*0x%x, attr=*0x%x)", scheduler, attr);

	if (!scheduler)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!scheduler.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuFinalizeScheduler(vm::ptr<CellFiberPpuScheduler> scheduler)
{
	cellFiber.todo("cellFiberPpuFinalizeScheduler(scheduler=*0x%x)", scheduler);

	if (!scheduler)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!scheduler.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuRunFibers(vm::ptr<CellFiberPpuScheduler> scheduler)
{
	cellFiber.todo("cellFiberPpuRunFibers(scheduler=*0x%x)", scheduler);

	if (!scheduler)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!scheduler.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuCheckFlags(vm::ptr<CellFiberPpuScheduler> scheduler)
{
	cellFiber.todo("cellFiberPpuCheckFlags(scheduler=*0x%x)", scheduler);

	if (!scheduler)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!scheduler.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuHasRunnableFiber(vm::ptr<CellFiberPpuScheduler> scheduler, vm::ptr<b8> flag)
{
	cellFiber.todo("cellFiberPpuHasRunnableFiber(scheduler=*0x%x, flag=*0x%x)", scheduler, flag);

	if (!scheduler || !flag)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!scheduler.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code _cellFiberPpuAttributeInitialize(vm::ptr<CellFiberPpuAttribute> attr, u32 sdkVersion)
{
	cellFiber.warning("_cellFiberPpuAttributeInitialize(attr=*0x%x, sdkVersion=0x%x)", attr, sdkVersion);

	if (!attr)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!attr.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	memset(attr.get_ptr(), 0, sizeof(CellFiberPpuAttribute));
	attr->onExitCallback = vm::null;

	return CELL_OK;
}

error_code cellFiberPpuCreateFiber(vm::ptr<CellFiberPpuScheduler> scheduler, vm::ptr<CellFiberPpu> fiber, vm::ptr<CellFiberPpuEntry> entry, u64 arg, u32 priority, vm::ptr<void> eaStack, u32 sizeStack, vm::cptr<CellFiberPpuAttribute> attr)
{
	cellFiber.todo("cellFiberPpuCreateFiber(scheduler=*0x%x, fiber=*0x%x, entry=*0x%x, arg=0x%x, priority=%d, eaStack=*0x%x, sizeStack=0x%x, attr=*0x%x)", scheduler, fiber, entry, arg, priority, eaStack, sizeStack, attr);

	if (!scheduler || !fiber || !entry || !eaStack)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!scheduler.aligned() || !fiber.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	if (priority > 3)
	{
		return CELL_FIBER_ERROR_INVAL;
	}

	return CELL_OK;
}

error_code cellFiberPpuExit(s32 status)
{
	cellFiber.todo("cellFiberPpuExit(status=%d)", status);

	return CELL_OK;
}

error_code cellFiberPpuYield()
{
	cellFiber.todo("cellFiberPpuYield()");

	return CELL_OK;
}

error_code cellFiberPpuJoinFiber(vm::ptr<CellFiberPpu> fiber, vm::ptr<s32> status)
{
	cellFiber.todo("cellFiberPpuJoinFiber(fiber=*0x%x, status=*0x%x)", fiber, status);

	if (!fiber || !status)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!fiber.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

vm::ptr<void> cellFiberPpuSelf()
{
	cellFiber.trace("cellFiberPpuSelf() -> nullptr"); // TODO

	// returns fiber structure (zero for simple PPU thread)
	return vm::null;
}

error_code cellFiberPpuSendSignal(vm::ptr<CellFiberPpu> fiber, vm::ptr<u32> numWorker)
{
	cellFiber.trace("cellFiberPpuSendSignal(fiber=*0x%x, numWorker=*0x%x)", fiber, numWorker);

	if (!fiber)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!fiber.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	{
		std::lock_guard lock(g_cf_mtx);
		if (auto it = g_cf_fibers.find(fiber.addr()); it != g_cf_fibers.end())
		{
			it->second->signals++;
			it->second->runnable = true;
		}
	}

	if (numWorker)
	{
		*numWorker = 0;
	}

	return CELL_OK;
}

error_code cellFiberPpuWaitSignal(ppu_thread& ppu)
{
	cellFiber.trace("cellFiberPpuWaitSignal()");

	// [DISASM PROBE] one-shot dump of MAIN's readiness-poll loop (main's Wakeup returns to 0x5b82e0, so its
	// sys_mutex_lock(0x85000300) poll is right after) so it can be disassembled offline to find the exact control
	// field/value main waits for. Dumped from the fiber thread, but vm memory (the code) is shared. Remove later.
	{
		static atomic_t<u32> s_dis{0};
		if (s_dis.compare_and_swap_test(0, 1))
		{
			std::string s;
			for (u32 a = 0x5b8100; a <= 0x5b8700; a += 4) s += fmt::format("%08x", +vm::read32(a));
			cellFiber.warning("[MAINDIS] base=0x5b8100 ops=%s", s.c_str());
		}
	}

	// thread-per-fiber: block THIS fiber's guest thread until it is signaled (cellFiberPpu[UtilWorkerControl]
	// SendSignal). Without this the fiber busy-loops (WaitSignal was a no-op stub) and never yields, so DC's
	// engine never makes forward progress. Locate the fiber by the calling thread id (recorded in CreateFiber).
	// NOTE: the fiber thread is auto-started inside sys_ppu_thread_create and can reach WaitSignal BEFORE
	// CreateFiber finishes recording rec->tid (confirmed by log ordering). So re-lookup the record on each
	// iteration until it appears, rather than giving up and busy-returning.
	std::shared_ptr<ppu_fiber_rec> rec;
	bool logged = false;

	while (!ppu.is_stopped())
	{
		if (!rec)
		{
			std::lock_guard lock(g_cf_mtx);
			for (auto& [addr, r] : g_cf_fibers)
			{
				if (r->tid.load() == ppu.id) { rec = r; break; }
			}
		}
		if (!logged)
		{
			static atomic_t<u32> s_w{0}; const u32 n = ++s_w;
			if (n <= 40u) cellFiber.warning("[FIBERWAIT] #%u tid=0x%x found=%d", n, ppu.id, rec ? 1 : 0);
			logged = true;
		}
		if (rec)
		{
			const u32 cur = rec->signals.load();
			if (cur > 0 && rec->signals.compare_and_swap_test(cur, cur - 1))
			{
				{ static atomic_t<u32> s_k{0}; const u32 n = ++s_k; if (n <= 40u) cellFiber.warning("[FIBERWAKE] tid=0x%x", ppu.id); }
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::microseconds(150));
	}

	return CELL_OK;
}

error_code cellFiberPpuWaitFlag(vm::ptr<u32> eaFlag, b8 flagValue)
{
	cellFiber.todo("cellFiberPpuWaitFlag(eaFlag=*0x%x, flagValue=%d)", eaFlag, flagValue);

	if (!eaFlag)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	return CELL_OK;
}

error_code cellFiberPpuGetScheduler(vm::ptr<CellFiberPpu> fiber, vm::pptr<CellFiberPpuScheduler> pScheduler)
{
	cellFiber.todo("cellFiberPpuGetScheduler(fiber=*0x%x, pScheduler=**0x%x)", fiber, pScheduler);

	if (!fiber || !pScheduler)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!fiber.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuSetPriority(u32 priority)
{
	cellFiber.todo("cellFiberPpuSetPriority(priority=%d)", priority);

	if (priority > 3)
	{
		return CELL_FIBER_ERROR_INVAL;
	}

	return CELL_OK;
}

error_code cellFiberPpuCheckStackLimit()
{
	cellFiber.todo("cellFiberPpuCheckStackLimit()");

	return CELL_OK;
}

error_code _cellFiberPpuContextAttributeInitialize(vm::ptr<CellFiberPpuContextAttribute> attr, u32 sdkVersion)
{
	cellFiber.warning("_cellFiberPpuContextAttributeInitialize(attr=*0x%x, sdkVersion=0x%x)", attr, sdkVersion);

	if (!attr)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!attr.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	memset(attr.get_ptr(), 0, sizeof(CellFiberPpuContextAttribute));
	attr->debuggerSupport = false;

	return CELL_OK;
}

error_code cellFiberPpuContextInitialize(vm::ptr<CellFiberPpuContext> context, vm::ptr<CellFiberPpuContextEntry> entry, u64 arg, vm::ptr<void> eaStack, u32 sizeStack, vm::cptr<CellFiberPpuContextAttribute> attr)
{
	cellFiber.todo("cellFiberPpuContextInitialize(context=*0x%x, entry=*0x%x, arg=0x%x, eaStack=*0x%x, sizeStack=0x%x, attr=*0x%x)", context, entry, arg, eaStack, sizeStack, attr);

	if (!context || !entry || !eaStack)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!context.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuContextFinalize(vm::ptr<CellFiberPpuContext> context)
{
	cellFiber.todo("cellFiberPpuContextFinalize(context=*0x%x)", context);

	if (!context)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!context.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuContextRun(vm::ptr<CellFiberPpuContext> context, vm::ptr<s32> cause, vm::pptr<CellFiberPpuContext> fiberFrom, vm::cptr<CellFiberPpuContextExecutionOption> option)
{
	cellFiber.todo("cellFiberPpuContextRun(context=*0x%x, cause=*0x%x, fiberFrom=**0x%x, option=*0x%x)", context, cause, fiberFrom, option);

	if (!context || !cause)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!context.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuContextSwitch(vm::ptr<CellFiberPpuContext> context, vm::pptr<CellFiberPpuContext> fiberFrom, vm::cptr<CellFiberPpuContextExecutionOption> option)
{
	cellFiber.todo("cellFiberPpuContextSwitch(context=*0x%x, fiberFrom=**0x%x, option=*0x%x)", context, fiberFrom, option);

	if (!context)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!context.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

vm::ptr<CellFiberPpuContext> cellFiberPpuContextSelf()
{
	cellFiber.todo("cellFiberPpuContextSelf()");

	return vm::null;
}

error_code cellFiberPpuContextReturnToThread(s32 cause)
{
	cellFiber.todo("cellFiberPpuContextReturnToThread(cause=%d)", cause);

	return CELL_OK;
}

error_code cellFiberPpuContextCheckStackLimit()
{
	cellFiber.todo("cellFiberPpuContextCheckStackLimit()");

	return CELL_OK;
}

error_code cellFiberPpuContextRunScheduler(vm::ptr<CellFiberPpuSchedulerCallback> scheduler, u64 arg0, u64 arg1, vm::ptr<s32> cause, vm::pptr<CellFiberPpuContext> fiberFrom, vm::cptr<CellFiberPpuContextExecutionOption> option)
{
	cellFiber.todo("cellFiberPpuContextRunScheduler(scheduler=*0x%x, arg0=0x%x, arg1=0x%x, cause=*0x%x, fiberFrom=**0x%x, option=*0x%x)", scheduler, arg0, arg1, cause, fiberFrom, option);

	if (!scheduler || !cause)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	return CELL_OK;
}

error_code cellFiberPpuContextEnterScheduler(vm::ptr<CellFiberPpuSchedulerCallback> scheduler, u64 arg0, u64 arg1, vm::pptr<CellFiberPpuContext> fiberFrom, vm::cptr<CellFiberPpuContextExecutionOption> option)
{
	cellFiber.todo("cellFiberPpuContextEnterScheduler(scheduler=*0x%x, arg0=0x%x, arg1=0x%x, fiberFrom=**0x%x, option=*0x%x)", scheduler, arg0, arg1, fiberFrom, option);

	if (!scheduler)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	return CELL_OK;
}

error_code cellFiberPpuSchedulerTraceInitialize(vm::ptr<CellFiberPpuScheduler> scheduler, vm::ptr<void> buffer, u32 size, u32 mode)
{
	cellFiber.todo("cellFiberPpuSchedulerTraceInitialize(scheduler=*0x%x, buffer=*0x%x, size=0x%x, mode=0x%x)", scheduler, buffer, size, mode);

	if (!scheduler || !buffer)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!scheduler.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuSchedulerTraceFinalize(vm::ptr<CellFiberPpuScheduler> scheduler)
{
	cellFiber.todo("cellFiberPpuSchedulerTraceFinalize(scheduler=*0x%x)", scheduler);

	if (!scheduler)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!scheduler.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuSchedulerTraceStart(vm::ptr<CellFiberPpuScheduler> scheduler)
{
	cellFiber.todo("cellFiberPpuSchedulerTraceStart(scheduler=*0x%x)", scheduler);

	if (!scheduler)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!scheduler.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuSchedulerTraceStop(vm::ptr<CellFiberPpuScheduler> scheduler)
{
	cellFiber.todo("cellFiberPpuSchedulerTraceStop(scheduler=*0x%x)", scheduler);

	if (!scheduler)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!scheduler.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code _cellFiberPpuUtilWorkerControlAttributeInitialize(vm::ptr<CellFiberPpuUtilWorkerControlAttribute> attr, u32 sdkVersion)
{
	cellFiber.warning("_cellFiberPpuUtilWorkerControlAttributeInitialize(attr=*0x%x, sdkVersion=0x%x)", attr, sdkVersion);

	if (!attr)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!attr.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	memset(attr.get_ptr(), 0, sizeof(CellFiberPpuUtilWorkerControlAttribute));
	attr->scheduler.autoCheckFlags = false;
	attr->scheduler.autoCheckFlagsIntervalUsec = 0;
	attr->scheduler.debuggerSupport = false;

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlRunFibers(ppu_thread& ppu, vm::ptr<CellFiberPpuUtilWorkerControl> control)
{
	cellFiber.warning("cellFiberPpuUtilWorkerControlRunFibers(control=*0x%x)", control);

	if (!control)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!control.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	std::shared_ptr<ppu_worker_ctl> ctl;
	{
		std::lock_guard lock(g_cf_mtx);
		if (auto it = g_cf_ctls.find(control.addr()); it != g_cf_ctls.end()) ctl = it->second;
	}
	if (!ctl)
	{
		return CELL_OK;
	}

	// Fibers run on their own dedicated guest threads (thread-per-fiber, see CreateFiber). The worker just parks
	// here so it doesn't fall through to its post-RunFibers shutdown path; it unparks on emu stop or control
	// shutdown. (In real cellFiber this call IS the cooperative scheduler; here the per-fiber threads are.)
	while (!ctl->shutdown && !ppu.is_stopped())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(8));
	}

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlInitialize(vm::ptr<CellFiberPpuUtilWorkerControl> control)
{
	cellFiber.todo("cellFiberPpuUtilWorkerControlInitialize(control=*0x%x)", control);

	if (!control)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!control.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlSetPollingMode(vm::ptr<CellFiberPpuUtilWorkerControl> control, s32 mode, s32 timeout)
{
	cellFiber.todo("cellFiberPpuUtilWorkerControlSetPollingMode(control=*0x%x, mode=%d, timeout=%d)", control, mode, timeout);

	if (!control)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!control.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlJoinFiber(vm::ptr<CellFiberPpuUtilWorkerControl> control, vm::ptr<CellFiberPpu> fiber, vm::ptr<s32> exitCode)
{
	cellFiber.todo("cellFiberPpuUtilWorkerControlJoinFiber(control=*0x%x, fiber=*0x%x, exitCode=*0x%x)", control, fiber, exitCode);

	if (!control)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!control.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlDisconnectEventQueue()
{
	cellFiber.todo("cellFiberPpuUtilWorkerControlDisconnectEventQueue()");

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlSendSignal(vm::ptr<CellFiberPpu> fiber, vm::ptr<u32> numWorker)
{
	cellFiber.trace("cellFiberPpuUtilWorkerControlSendSignal(fiber=*0x%x, numWorker=*0x%x)", fiber, numWorker);

	if (!fiber)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!fiber.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	bool found = false;
	{
		std::lock_guard lock(g_cf_mtx);
		if (auto it = g_cf_fibers.find(fiber.addr()); it != g_cf_fibers.end())
		{
			it->second->signals++;
			it->second->runnable = true;
			found = true;
		}
	}
	{ static atomic_t<u32> s_sig{0}; const u32 n = ++s_sig; if (n <= 40u) cellFiber.warning("[FIBERSIG] #%u fiber=0x%x found=%d", n, fiber.addr(), found ? 1 : 0); }

	if (numWorker)
	{
		*numWorker = 0;
	}

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlConnectEventQueueToSpurs()
{
	cellFiber.todo("cellFiberPpuUtilWorkerControlConnectEventQueueToSpurs()");

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlFinalize(vm::ptr<CellFiberPpuUtilWorkerControl> control)
{
	cellFiber.todo("cellFiberPpuUtilWorkerControlFinalize(control=*0x%x)", control);

	if (!control)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!control.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlWakeup(vm::ptr<CellFiberPpuUtilWorkerControl> control)
{
	cellFiber.todo("cellFiberPpuUtilWorkerControlWakeup(control=*0x%x)", control);

	if (!control)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!control.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

// HLE worker-thread entry (thread-per-fiber). Runs ONE DC fiber on its own guest ppu_thread: when the fiber is
// runnable, run its guest entry on this thread's stack. If the entry returns (a "task" fiber) we park until it is
// re-signaled (SendSignal sets runnable); if the entry loops forever (a "service" fiber) it simply runs here for
// the thread's lifetime. Either way a blocked/looping fiber no longer stalls the cellFiber worker, and fibers
// coordinate with each other and the game via DC's own sync (sema/event/lwmutex).
void cellFiberWorkerThreadEntry(ppu_thread& ppu, vm::ptr<CellFiberPpu> fiber)
{
	std::shared_ptr<ppu_fiber_rec> rec;
	{
		std::lock_guard lock(g_cf_mtx);
		if (auto it = g_cf_fibers.find(fiber.addr()); it != g_cf_fibers.end()) rec = it->second;
	}
	if (!rec)
	{
		cellFiber.error("[FIBERTHREAD] no record for fiber=0x%x", fiber.addr());
		return;
	}
	cellFiber.warning("[FIBERTHREAD] start fiber=0x%x entry=0x%x arg=0x%x", fiber.addr(), rec->entry.addr(), rec->arg);

	while (!ppu.is_stopped())
	{
		if (rec->runnable.exchange(false))
		{
			rec->running = true;
			{ static atomic_t<u32> s_run{0}; const u32 n = ++s_run; if (n <= 60u) cellFiber.warning("[FIBERRUN] #%u fiber=0x%x entry=0x%x", n, fiber.addr(), rec->entry.addr()); }
			rec->entry(ppu, rec->arg); // run the guest fiber entry
			rec->running = false;
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
	}
}

error_code cellFiberPpuUtilWorkerControlCreateFiber(ppu_thread& ppu, vm::ptr<CellFiberPpuUtilWorkerControl> control, vm::ptr<CellFiberPpu> fiber, vm::ptr<CellFiberPpuEntry> entry, u64 arg, u32 priority, vm::ptr<void> eaStack, u32 sizeStack, vm::cptr<CellFiberPpuAttribute> attr)
{
	cellFiber.warning("cellFiberPpuUtilWorkerControlCreateFiber(control=*0x%x, fiber=*0x%x, entry=*0x%x, arg=0x%x, priority=%d, eaStack=*0x%x, sizeStack=0x%x, attr=*0x%x)", control, fiber, entry, arg, priority, eaStack, sizeStack, attr);

	if (!control || !fiber || !entry)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!control.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	auto rec = std::make_shared<ppu_fiber_rec>();
	rec->entry        = entry;
	rec->arg          = arg;
	rec->control_addr = control.addr();
	rec->stack_ea     = eaStack.addr();
	rec->stack_size   = sizeStack;
	rec->runnable     = true; // a newly-created fiber is ready to run
	{
		std::lock_guard lock(g_cf_mtx);
		g_cf_fibers[fiber.addr()] = rec;
		auto& ctl = g_cf_ctls[control.addr()];
		if (!ctl) ctl = std::make_shared<ppu_worker_ctl>();
		ctl->fiber_addrs.push_back(fiber.addr());
	}

	// Spawn a dedicated guest ppu_thread that runs this fiber's GUEST entry directly. NOTE: the WASM build
	// redirects HLE-area thread entries (rtoc==0) to a return-stub (see _sys_ppu_thread_create), so an HLE
	// wrapper as the thread entry would immediately return — we must start the thread at the real guest fiber
	// entry (valid rtoc). A service-style fiber loops here for the thread's lifetime; fibers coordinate with
	// each other and the game via DC's own sync (sema/event/lwmutex) and via cellFiberPpuWaitSignal/SendSignal.
	const vm::var<char[]> _name = vm::make_str("cellFiberPpu");
	vm::var<u64> tid;
	const u32 ssz  = (sizeStack >= 0x4000u) ? sizeStack : 0x10000u;
	const s32 prio = static_cast<s32>(priority <= 3071u ? priority : 1000u);
	ppu_execute<&sys_ppu_thread_create>(ppu, +tid, entry.addr(), arg, prio, ssz, SYS_PPU_THREAD_CREATE_JOINABLE, +_name);
	const auto tid_ptr = +tid;
	const u32 new_tid  = static_cast<u32>(*tid_ptr);
	rec->tid = new_tid; // so cellFiberPpuWaitSignal (called by this guest thread) can locate this fiber
	cellFiber.warning("[FIBERSPAWN] fiber=0x%x entry=0x%x arg=0x%x ssz=0x%x prio=%d tid=0x%x", fiber.addr(), entry.addr(), arg, ssz, prio, new_tid);

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlShutdown(vm::ptr<CellFiberPpuUtilWorkerControl> control)
{
	cellFiber.todo("cellFiberPpuUtilWorkerControlShutdown(control=*0x%x)", control);

	if (!control)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!control.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlCheckFlags(vm::ptr<CellFiberPpuUtilWorkerControl> control, b8 wakingUp)
{
	cellFiber.todo("cellFiberPpuUtilWorkerControlCheckFlags(control=*0x%x, wakingUp=%d)", control, wakingUp);

	if (!control)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!control.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	return CELL_OK;
}

error_code cellFiberPpuUtilWorkerControlInitializeWithAttribute(vm::ptr<CellFiberPpuUtilWorkerControl> control, vm::ptr<CellFiberPpuUtilWorkerControlAttribute> attr)
{
	cellFiber.warning("cellFiberPpuUtilWorkerControlInitializeWithAttribute(control=*0x%x, attr=*0x%x)", control, attr);

	if (!control)
	{
		return CELL_FIBER_ERROR_NULL_POINTER;
	}

	if (!control.aligned())
	{
		return CELL_FIBER_ERROR_ALIGN;
	}

	std::lock_guard lock(g_cf_mtx);
	if (!g_cf_ctls[control.addr()]) g_cf_ctls[control.addr()] = std::make_shared<ppu_worker_ctl>();

	return CELL_OK;
}

DECLARE(ppu_module_manager::cellFiber)("cellFiber", []()
{
	// Internal HLE thread-entry used by thread-per-fiber CreateFiber (must be registered so func_addr resolves).
	REG_HIDDEN_FUNC(cellFiberWorkerThreadEntry);

	REG_FUNC(cellFiber, _cellFiberPpuInitialize);

	REG_FUNC(cellFiber, _cellFiberPpuSchedulerAttributeInitialize);
	REG_FUNC(cellFiber, cellFiberPpuInitializeScheduler);
	REG_FUNC(cellFiber, cellFiberPpuFinalizeScheduler);
	REG_FUNC(cellFiber, cellFiberPpuRunFibers);
	REG_FUNC(cellFiber, cellFiberPpuCheckFlags);
	REG_FUNC(cellFiber, cellFiberPpuHasRunnableFiber);

	REG_FUNC(cellFiber, _cellFiberPpuAttributeInitialize);
	REG_FUNC(cellFiber, cellFiberPpuCreateFiber);
	REG_FUNC(cellFiber, cellFiberPpuExit);
	REG_FUNC(cellFiber, cellFiberPpuYield);
	REG_FUNC(cellFiber, cellFiberPpuJoinFiber);
	REG_FUNC(cellFiber, cellFiberPpuSelf);
	REG_FUNC(cellFiber, cellFiberPpuSendSignal);
	REG_FUNC(cellFiber, cellFiberPpuWaitSignal);
	REG_FUNC(cellFiber, cellFiberPpuWaitFlag);
	REG_FUNC(cellFiber, cellFiberPpuGetScheduler);
	REG_FUNC(cellFiber, cellFiberPpuSetPriority);
	REG_FUNC(cellFiber, cellFiberPpuCheckStackLimit);

	REG_FUNC(cellFiber, _cellFiberPpuContextAttributeInitialize);
	REG_FUNC(cellFiber, cellFiberPpuContextInitialize);
	REG_FUNC(cellFiber, cellFiberPpuContextFinalize);
	REG_FUNC(cellFiber, cellFiberPpuContextRun);
	REG_FUNC(cellFiber, cellFiberPpuContextSwitch);
	REG_FUNC(cellFiber, cellFiberPpuContextSelf);
	REG_FUNC(cellFiber, cellFiberPpuContextReturnToThread);
	REG_FUNC(cellFiber, cellFiberPpuContextCheckStackLimit);

	REG_FUNC(cellFiber, cellFiberPpuContextRunScheduler);
	REG_FUNC(cellFiber, cellFiberPpuContextEnterScheduler);

	REG_FUNC(cellFiber, cellFiberPpuSchedulerTraceInitialize);
	REG_FUNC(cellFiber, cellFiberPpuSchedulerTraceFinalize);
	REG_FUNC(cellFiber, cellFiberPpuSchedulerTraceStart);
	REG_FUNC(cellFiber, cellFiberPpuSchedulerTraceStop);

	REG_FUNC(cellFiber, _cellFiberPpuUtilWorkerControlAttributeInitialize);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlRunFibers);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlInitialize);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlSetPollingMode);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlJoinFiber);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlDisconnectEventQueue);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlSendSignal);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlConnectEventQueueToSpurs);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlFinalize);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlWakeup);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlCreateFiber);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlShutdown);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlCheckFlags);
	REG_FUNC(cellFiber, cellFiberPpuUtilWorkerControlInitializeWithAttribute);
});
