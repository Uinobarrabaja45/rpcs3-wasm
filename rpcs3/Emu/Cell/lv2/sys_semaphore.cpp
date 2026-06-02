#include "stdafx.h"
#include "sys_semaphore.h"

#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"

#include "util/asm.hpp"

LOG_CHANNEL(sys_semaphore);

lv2_sema::lv2_sema(utils::serial& ar)
	: protocol(ar)
	, key(ar)
	, name(ar)
	, max(ar)
{
	ar(val);
}

std::function<void(void*)> lv2_sema::load(utils::serial& ar)
{
	return load_func(make_shared<lv2_sema>(stx::exact_t<utils::serial&>(ar)));
}

void lv2_sema::save(utils::serial& ar)
{
	USING_SERIALIZATION_VERSION(lv2_sync);
	ar(protocol, key, name, max, std::max<s32>(+val, 0));
}

error_code sys_semaphore_create(ppu_thread& ppu, vm::ptr<u32> sem_id, vm::ptr<sys_semaphore_attribute_t> attr, s32 initial_val, s32 max_val)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_create(sem_id=*0x%x, attr=*0x%x, initial_val=%d, max_val=%d)", sem_id, attr, initial_val, max_val);

	if (!sem_id || !attr)
	{
		return CELL_EFAULT;
	}

	if (max_val <= 0 || initial_val > max_val || initial_val < 0)
	{
		sys_semaphore.error("sys_semaphore_create(): invalid parameters (initial_val=%d, max_val=%d)", initial_val, max_val);
		return CELL_EINVAL;
	}

	const auto _attr = *attr;

	const u32 protocol = _attr.protocol;

	if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_PRIORITY)
	{
		sys_semaphore.error("sys_semaphore_create(): unknown protocol (0x%x)", protocol);
		return CELL_EINVAL;
	}

	const u64 ipc_key = lv2_obj::get_key(_attr);

	if (ipc_key)
	{
		sys_semaphore.warning("sys_semaphore_create(sem_id=*0x%x, attr=*0x%x, initial_val=%d, max_val=%d): IPC=0x%016x", sem_id, attr, initial_val, max_val, ipc_key);
	}

	if (auto error = lv2_obj::create<lv2_sema>(_attr.pshared, ipc_key, _attr.flags, [&]
	{
		return make_shared<lv2_sema>(protocol, ipc_key, _attr.name_u64, max_val, initial_val);
	}))
	{
		return error;
	}

	ppu.check_state();

	*sem_id = idm::last_id();
#ifdef __EMSCRIPTEN__
	printf("[SEMCREATE] id=0x%x init=%d max=%d\n", static_cast<unsigned>(idm::last_id()), static_cast<int>(initial_val), static_cast<int>(max_val));
#endif
	return CELL_OK;
}

error_code sys_semaphore_destroy(ppu_thread& ppu, u32 sem_id)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_destroy(sem_id=0x%x)", sem_id);

	const auto sem = idm::withdraw<lv2_obj, lv2_sema>(sem_id, [](lv2_sema& sema) -> CellError
	{
		if (sema.val < 0)
		{
			return CELL_EBUSY;
		}

		lv2_obj::on_id_destroy(sema, sema.key);
		return {};
	});

	if (!sem)
	{
		return CELL_ESRCH;
	}

	if (sem->key)
	{
		sys_semaphore.warning("sys_semaphore_destroy(sem_id=0x%x): IPC=0x%016x", sem_id, sem->key);
	}

	if (sem.ret)
	{
		return sem.ret;
	}

	return CELL_OK;
}

error_code sys_semaphore_wait(ppu_thread& ppu, u32 sem_id, u64 timeout)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_wait(sem_id=0x%x, timeout=0x%llx)", sem_id, timeout);
#ifdef __EMSCRIPTEN__
	// Terraria's EE ResourceLoop (0x1000006) waits on its work-sem (0x96006700) at
	// lr=0x423c00 with timeout=0. A prior no-op returned CELL_OK here, but that turned
	// a clean block into a 1.26M-iteration HOT SPIN (no work to do on wake) that flooded
	// the log ~127x over the ring buffer and contended on lv2 locks. Let it BLOCK
	// naturally (park, yield CPU) so main isn't starved; log once. The real issue is the
	// dead producer of 0x96006700 (see SEMPOST probe).
	if (ppu.id == 0x1000006u && static_cast<unsigned>(ppu.lr) == 0x423c00u)
	{
		static thread_local bool s_lblk = false;
		if (!s_lblk) { s_lblk = true; printf("[LOADERBLK] loader parks on work-sem=0x%x timeout=0x%llx (was hot-spinning)\n", static_cast<unsigned>(sem_id), static_cast<unsigned long long>(timeout)); }
		// fall through to the real (blocking) wait — do NOT hot-spin
	}
#endif

	const auto sem = idm::get<lv2_obj, lv2_sema>(sem_id, [&, notify = lv2_obj::notify_all_t()](lv2_sema& sema)
	{
#ifdef __EMSCRIPTEN__
		// REACHABILITY EXPERIMENT (2026-06-02): main (0x1000000) is stuck in a task-completion
		// BARRIER loop (game code @0x411168) that calls sys_semaphore_wait(0x96004600) once per
		// dispatched resource-decode task and exits only after N completions. Those completions
		// never arrive (the SPU/SPURS decode pipeline doesn't run to completion in this wasm port),
		// so main crawls forever (each wait relies on the 9s WAITFORCE timeout). Short-circuit
		// ONLY main's waits on THIS barrier sem so main exits the barrier and runs its render code
		// — to learn whether the 505 splash is then issued as draws (WGPU-DRAW) or whether main
		// faults on unloaded resources. This is a probe of reachability, NOT a correctness fix.
		if (ppu.id == 0x1000000u && sem_id == 0x96004600u)
		{
			// One-shot: dump the barrier struct so we know HOW MANY completions main waits for.
			// game loop @0x411168: limit=*(r31+0x104), counter=*(r31+0x108), sem=*(r31+0x114).
			static thread_local bool s_bd = false;
			if (!s_bd)
			{
				s_bd = true;
				const u32 r31 = static_cast<u32>(ppu.gpr[31]);
				const auto rd = [](u32 a) -> u32 { return (a >= 0x10000u && a < 0x60000000u) ? reinterpret_cast<be_t<u32>*>(static_cast<u8*>(vm::base(a)))->value() : 0xBADBADu; };
				printf("[BARRIER] r31=0x%x limit(+0x104)=%d counter(+0x108)=%d sem(+0x114)=0x%x r30=0x%x lr=0x%x\n",
					r31, static_cast<int>(rd(r31 + 0x104)), static_cast<int>(rd(r31 + 0x108)), static_cast<unsigned>(rd(r31 + 0x114)), static_cast<unsigned>(ppu.gpr[30]), static_cast<unsigned>(ppu.lr));
				// Dump the full barrier/work-descriptor struct so we can identify WHAT async op main
				// dispatched (its function ptr / args) and therefore which thread should post idx6.
				for (u32 o = 0; o <= 0x120u; o += 16)
					printf("  [BARRIER+] @+0x%x: %08x %08x %08x %08x\n", o, rd(r31 + o), rd(r31 + o + 4), rd(r31 + o + 8), rd(r31 + o + 0xc));
			}
			// (barrier-skip DISABLED — let main wait on idx6 normally so we can verify the SPU
			//  group-start fix makes the decode task post idx6 naturally; fall through to real wait.)
		}
#endif
		const s32 val = sema.val;

		if (val > 0)
		{
			if (sema.val.compare_and_swap_test(val, val - 1))
			{
				return true;
			}
		}

		lv2_obj::prepare_for_sleep(ppu);

		std::lock_guard lock(sema.mutex);

		if (sema.val-- <= 0)
		{
#ifdef __EMSCRIPTEN__
			if ((ppu.id & 0xFF000000u) == 0x01000000u) printf("[SEMBLOCK] tid=0x%x sem=0x%x BLOCKS val=%d lr=0x%x cia=0x%x\n", static_cast<unsigned>(ppu.id), static_cast<unsigned>(sem_id), static_cast<int>(sema.val.load()), static_cast<unsigned>(ppu.lr), static_cast<unsigned>(ppu.cia));
#endif
			sema.sleep(ppu, timeout);
			lv2_obj::emplace(sema.sq, &ppu);
			return false;
		}

		return true;
	});

	if (!sem)
	{
		return CELL_ESRCH;
	}

	if (sem.ret)
	{
		return CELL_OK;
	}

	ppu.gpr[3] = CELL_OK;

	while (auto state = +ppu.state)
	{
		if (state & cpu_flag::signal && ppu.state.test_and_reset(cpu_flag::signal))
		{
			break;
		}

		if (is_stopped(state))
		{
			std::lock_guard lock(sem->mutex);

			for (auto cpu = +sem->sq; cpu; cpu = cpu->next_cpu)
			{
				if (cpu == &ppu)
				{
					ppu.state += cpu_flag::again;
					return {};
				}
			}

			break;
		}

		for (usz i = 0; cpu_flag::signal - ppu.state && i < 50; i++)
		{
			busy_wait(500);
		}

		if (ppu.state & cpu_flag::signal)
 		{
			continue;
		}

		if (timeout)
		{
			if (lv2_obj::wait_timeout(timeout, &ppu))
			{
				// Wait for rescheduling
				if (ppu.check_state())
				{
					continue;
				}

				ppu.state += cpu_flag::wait;

				std::lock_guard lock(sem->mutex);

				if (!sem->unqueue(sem->sq, &ppu))
				{
					break;
				}

				ensure(0 > sem->val.fetch_op([](s32& val)
				{
					if (val < 0)
					{
						val++;
					}
				}));

				ppu.gpr[3] = CELL_ETIMEDOUT;
				break;
			}
		}
		else
		{
#ifdef __EMSCRIPTEN__
			ppu.state.wait(state, atomic_wait_timeout{15'000'000});
			// Check if we have been unqueued (meaning we were signaled)
			{
				std::lock_guard lock(sem->mutex);
				bool queued = false;
				for (auto cpu = +sem->sq; cpu; cpu = cpu->next_cpu)
				{
					if (cpu == &ppu)
					{
						queued = true;
						break;
					}
				}
				if (!queued)
				{
					ppu.gpr[3] = CELL_OK;
					break;
				}
			}
			{ static thread_local u32 s_smf = 0; if ((ppu.id == 0x1000000u || ppu.id == 0x1000006u) && ++s_smf > 600u) { s_smf = 0; printf("[WAITFORCE] sem force tid=0x%x id=0x%x (re-check queue)\n", static_cast<unsigned>(ppu.id), static_cast<unsigned>(sem_id)); ppu.gpr[3] = CELL_OK; break; } }
#else
			ppu.state.wait(state);
#endif
		}
	}

	return not_an_error(ppu.gpr[3]);
}

error_code sys_semaphore_trywait(ppu_thread& ppu, u32 sem_id)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_trywait(sem_id=0x%x)", sem_id);

	const auto sem = idm::check<lv2_obj, lv2_sema>(sem_id, [&](lv2_sema& sema)
	{
		return sema.val.try_dec(0);
	});

	if (!sem)
	{
		return CELL_ESRCH;
	}

	if (!sem.ret)
	{
		return not_an_error(CELL_EBUSY);
	}

	return CELL_OK;
}

error_code sys_semaphore_post(ppu_thread& ppu, u32 sem_id, s32 count)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_post(sem_id=0x%x, count=%d)", sem_id, count);
#ifdef __EMSCRIPTEN__
	// Log every post to the game's semaphore pool (0x9600xxxx) EXCEPT the noisy
	// work-queue 0x96006800, so we can see who (if anyone) posts main's completion
	// sem 0x96004600 and the audio sem 0x96004100 — the two ends of the boot wall.
	if (sem_id != 0x96006800u && (sem_id & 0xFFFF0000u) == 0x96000000u) printf("[SEMPOST] sem=0x%x cnt=%d tid=0x%x lr=0x%x cia=0x%x\n", static_cast<unsigned>(sem_id), static_cast<int>(count), static_cast<unsigned>(ppu.id), static_cast<unsigned>(ppu.lr), static_cast<unsigned>(ppu.cia));
#endif

	const auto sem = idm::get<lv2_obj, lv2_sema>(sem_id, [&](lv2_sema& sema)
	{
		const s32 val = sema.val;

		if (val >= 0 && count > 0 && count <= sema.max - val)
		{
			if (sema.val.compare_and_swap_test(val, val + count))
			{
				return true;
			}
		}

		return false;
	});

	if (!sem)
	{
		return CELL_ESRCH;
	}

	if (count <= 0)
	{
		return CELL_EINVAL;
	}

	lv2_obj::notify_all_t notify;

	if (sem.ret)
	{
		return CELL_OK;
	}
	else
	{
		std::lock_guard lock(sem->mutex);

		for (auto cpu = +sem->sq; cpu; cpu = cpu->next_cpu)
		{
			if (static_cast<ppu_thread*>(cpu)->state & cpu_flag::again)
			{
				ppu.state += cpu_flag::again;
				return {};
			}
		}

		const auto [val, ok] = sem->val.fetch_op([&](s32& val)
		{
			if (count + 0u <= sem->max + 0u - val)
			{
				val += count;
				return true;
			}

			return false;
		});

		if (!ok)
		{
			return not_an_error(CELL_EBUSY);
		}

		// Wake threads
		const s32 to_awake = std::min<s32>(-std::min<s32>(val, 0), count);

		for (s32 i = 0; i < to_awake; i++)
		{
			sem->append((ensure(sem->schedule<ppu_thread>(sem->sq, sem->protocol))));
		}

		if (to_awake > 0)
		{
#ifdef __EMSCRIPTEN__
			printf("[SEMWAKE] sem=0x%x cnt=%d awake=%d tid=0x%x lr=0x%x\n", static_cast<unsigned>(sem_id), static_cast<int>(count), static_cast<int>(to_awake), static_cast<unsigned>(ppu.id), static_cast<unsigned>(ppu.lr));
#endif
			lv2_obj::awake_all();
		}
	}

	return CELL_OK;
}

error_code sys_semaphore_get_value(ppu_thread& ppu, u32 sem_id, vm::ptr<s32> count)
{
	ppu.state += cpu_flag::wait;

	sys_semaphore.trace("sys_semaphore_get_value(sem_id=0x%x, count=*0x%x)", sem_id, count);

	const auto sema = idm::check<lv2_obj, lv2_sema>(sem_id, [](lv2_sema& sema)
	{
		return std::max<s32>(0, sema.val);
	});

	if (!sema)
	{
		return CELL_ESRCH;
	}

	if (!count)
	{
		return CELL_EFAULT;
	}

	ppu.check_state();

	*count = sema.ret;
	return CELL_OK;
}
