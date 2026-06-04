#include "stdafx.h"
#include "Utilities/JIT.h"
#include "Utilities/StrUtil.h"
#include "util/serialization.hpp"
#include "Crypto/sha1.h"
#include "Crypto/unself.h"
#include "Loader/ELF.h"
#include "Loader/mself.hpp"
#include "Emu/localized_string.h"
#include "Emu/perf_meter.hpp"
#include "Emu/Memory/vm_reservation.h"
#include "Emu/Memory/vm_locking.h"
#include "Emu/RSX/Core/RSXReservationLock.hpp"
#include "Emu/VFS.h"
#include "Emu/system_progress.hpp"
#include "Emu/system_utils.hpp"
#include "Emu/System.h"
#include "PPUThread.h"
#include "PPUInterpreter.h"
#include "PPUAnalyser.h"
#include "PPUModule.h"
#include "PPUDisAsm.h"
#include "SPURecompiler.h"
#include "timers.hpp"
#include "lv2/sys_sync.h"
#include "lv2/sys_prx.h"
#include "lv2/sys_overlay.h"
#include "lv2/sys_process.h"
#include "lv2/sys_spu.h"

#ifdef LLVM_AVAILABLE
#ifdef _MSC_VER
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#include "PPUTranslator.h"
#endif

#include <cfenv>
#include <cctype>
#include <span>
#include <optional>
#include <charconv>

#include "util/asm.hpp"
#include "util/vm.hpp"
#include "util/v128.hpp"
#include "util/simd.hpp"
#include "util/sysinfo.hpp"

#include "Utilities/sema.h"

#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#endif

extern atomic_t<u64> g_watchdog_hold_ctr;

// Should be of the same type
using spu_rdata_t = decltype(ppu_thread::rdata);

extern void mov_rdata(spu_rdata_t& _dst, const spu_rdata_t& _src);
extern void mov_rdata_nt(spu_rdata_t& _dst, const spu_rdata_t& _src);
extern bool cmp_rdata(const spu_rdata_t& _lhs, const spu_rdata_t& _rhs);

// Verify AVX availability for TSX transactions
static const bool s_tsx_avx = utils::has_avx();

template <>
void fmt_class_string<ppu_join_status>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](ppu_join_status js)
	{
		switch (js)
		{
		case ppu_join_status::joinable: return "none";
		case ppu_join_status::detached: return "detached";
		case ppu_join_status::zombie: return "zombie";
		case ppu_join_status::exited: return "exited";
		case ppu_join_status::max: break;
		}

		return unknown;
	});
}

template <>
void fmt_class_string<ppu_thread_status>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](ppu_thread_status s)
	{
		switch (s)
		{
		case PPU_THREAD_STATUS_IDLE: return "IDLE";
		case PPU_THREAD_STATUS_RUNNABLE: return "RUN";
		case PPU_THREAD_STATUS_ONPROC: return "ONPROC";
		case PPU_THREAD_STATUS_SLEEP: return "SLEEP";
		case PPU_THREAD_STATUS_STOP: return "STOP";
		case PPU_THREAD_STATUS_ZOMBIE: return "Zombie";
		case PPU_THREAD_STATUS_DELETED: return "Deleted";
		case PPU_THREAD_STATUS_UNKNOWN: break;
		}

		return unknown;
	});
}

template <>
void fmt_class_string<typename ppu_thread::call_history_t>::format(std::string& out, u64 arg)
{
	const auto& history = get_object(arg);

	PPUDisAsm dis_asm(cpu_disasm_mode::normal, vm::g_sudo_addr);

	for (u64 count = 0, idx = history.index - 1; idx != umax && count < history.data.size(); count++, idx--)
	{
		const u32 pc = history.data[idx % history.data.size()];
		dis_asm.disasm(pc);
		fmt::append(out, "\n(%u) 0x%08x: %s", count, pc, dis_asm.last_opcode);
	}
}

template <>
void fmt_class_string<typename ppu_thread::syscall_history_t>::format(std::string& out, u64 arg)
{
	const auto& history = get_object(arg);

	for (u64 count = 0, idx = history.index - 1; idx != umax && count < history.data.size(); count++, idx--)
	{
		const auto& entry = history.data[idx % history.data.size()];
		fmt::append(out, "\n(%u) 0x%08x: %s, 0x%x, r3=0x%x, r4=0x%x, r5=0x%x, r6=0x%x", count, entry.cia, entry.func_name, entry.error, entry.args[0], entry.args[1], entry.args[2], entry.args[3]);
	}
}

extern const ppu_decoder<ppu_itype> g_ppu_itype{};
extern const ppu_decoder<ppu_iname> g_ppu_iname{};

#if defined(__EMSCRIPTEN__)
// wasm32 null-TOC cascade fix. main_thread starts in LLE liblv2 (valid TOC),
// then hands off to the EBOOT — but r2(TOC) arrives as 0. Once zero, the
// ELFv1 `std r2,0x28(r1)` / `ld r2,0x28(r1)` save/restore only re-propagates
// 0 (the on-stack saved TOC is already 0), so every TOC-relative global load
// reads 0, object chains collapse to null, and indirect calls land at cia=0.
// r2 is a per-module CONSTANT (the EBOOT's .toc pointer), so the dispatch
// loop simply re-establishes it whenever EBOOT code executes with r2==0.
// These are captured once in ppu_load_exec (from the OPD at e_entry + the
// EBOOT code segment) before any PPU thread runs.
atomic_t<u32> g_eboot_toc = 0;
atomic_t<u32> g_eboot_code_beg = 0;
atomic_t<u32> g_eboot_code_end = 0;
atomic_t<u32> g_main_cia_latch = 0; // last interpreter cia executed by main(0x1000000); frozen value = where main's interpreter is parked (e.g. an HLE call site)
#endif

template <>
bool serialize<ppu_thread::cr_bits>(utils::serial& ar, typename ppu_thread::cr_bits& o)
{
	if (ar.is_writing())
	{
		ar(o.pack());
	}
	else
	{
		o.unpack(ar);
	}

	return true;
}

extern void ppu_initialize();
extern void ppu_finalize(const ppu_module<lv2_obj>& info, bool force_mem_release = false);
extern bool ppu_initialize(const ppu_module<lv2_obj>& info, bool check_only = false, u64 file_size = 0);
static void ppu_initialize2(class jit_compiler& jit, const ppu_module<lv2_obj>& module_part, const std::string& cache_path, const std::string& obj_name);
extern bool ppu_load_exec(const ppu_exec_object&, bool virtual_load, const std::string&, utils::serial* = nullptr);
extern std::pair<shared_ptr<lv2_overlay>, CellError> ppu_load_overlay(const ppu_exec_object&, bool virtual_load, const std::string& path, s64 file_offset, utils::serial* = nullptr);
extern void ppu_unload_prx(const lv2_prx&);
extern shared_ptr<lv2_prx> ppu_load_prx(const ppu_prx_object&, bool virtual_load, const std::string&, s64 file_offset, utils::serial* = nullptr);
extern void ppu_execute_syscall(ppu_thread& ppu, u64 code);
static void ppu_break(ppu_thread&, ppu_opcode_t, be_t<u32>*, ppu_intrp_func*);

extern void do_cell_atomic_128_store(u32 addr, const void* to_write);

const auto ppu_gateway = build_function_asm<void(*)(ppu_thread*)>("ppu_gateway", [](native_asm& c, auto& args)
{
	// Gateway for PPU, converts from native to GHC calling convention, also saves RSP value for escape
	using namespace asmjit;

#if defined(ARCH_X64)
#ifdef _WIN32
	c.push(x86::r15);
	c.push(x86::r14);
	c.push(x86::r13);
	c.push(x86::r12);
	c.push(x86::rsi);
	c.push(x86::rdi);
	c.push(x86::rbp);
	c.push(x86::rbx);
	c.sub(x86::rsp, 0xc8);
	c.movaps(x86::oword_ptr(x86::rsp, 0xb0), x86::xmm15);
	c.movaps(x86::oword_ptr(x86::rsp, 0xa0), x86::xmm14);
	c.movaps(x86::oword_ptr(x86::rsp, 0x90), x86::xmm13);
	c.movaps(x86::oword_ptr(x86::rsp, 0x80), x86::xmm12);
	c.movaps(x86::oword_ptr(x86::rsp, 0x70), x86::xmm11);
	c.movaps(x86::oword_ptr(x86::rsp, 0x60), x86::xmm10);
	c.movaps(x86::oword_ptr(x86::rsp, 0x50), x86::xmm9);
	c.movaps(x86::oword_ptr(x86::rsp, 0x40), x86::xmm8);
	c.movaps(x86::oword_ptr(x86::rsp, 0x30), x86::xmm7);
	c.movaps(x86::oword_ptr(x86::rsp, 0x20), x86::xmm6);
#else
	c.push(x86::rbp);
	c.push(x86::r15);
	c.push(x86::r14);
	c.push(x86::r13);
	c.push(x86::r12);
	c.push(x86::rbx);
	c.push(x86::rax);
#endif

	// Save native stack pointer for longjmp emulation
	c.mov(x86::qword_ptr(args[0], ::offset32(&ppu_thread::hv_ctx, &rpcs3::hypervisor_context_t::regs)), x86::rsp);

	// Initialize args
	c.movabs(x86::r13, reinterpret_cast<u64>(&vm::g_exec_addr));
	c.mov(x86::r13, x86::qword_ptr(x86::r13));
	c.mov(x86::rbp, args[0]);
	c.mov(x86::edx, x86::dword_ptr(x86::rbp, ::offset32(&ppu_thread::cia))); // Load PC

	c.mov(x86::rax, x86::qword_ptr(x86::r13, x86::rdx, 1, 0)); // Load call target
	c.movabs(x86::r12, vm::g_exec_addr_seg_offset);
	c.add(x86::r12, x86::r13);
	c.shr(x86::edx, 1);
	c.mov(x86::edx, x86::word_ptr(x86::r12, x86::edx)); // Load relocation base
	c.shl(x86::edx, 13);
	c.mov(x86::r12d, x86::edx); // Set relocation base

	c.movabs(x86::rbx, reinterpret_cast<u64>(&vm::g_base_addr));
	c.mov(x86::rbx, x86::qword_ptr(x86::rbx));
	c.mov(x86::r14, x86::qword_ptr(x86::rbp, ::offset32(&ppu_thread::gpr, 0))); // Load some registers
	c.mov(x86::rsi, x86::qword_ptr(x86::rbp, ::offset32(&ppu_thread::gpr, 1)));
	c.mov(x86::rdi, x86::qword_ptr(x86::rbp, ::offset32(&ppu_thread::gpr, 2)));

	if (utils::has_avx())
	{
		c.vzeroupper();
	}

	c.call(x86::rax);

	if (utils::has_avx())
	{
		c.vzeroupper();
	}

#ifdef _WIN32
	c.movaps(x86::xmm6, x86::oword_ptr(x86::rsp, 0x20));
	c.movaps(x86::xmm7, x86::oword_ptr(x86::rsp, 0x30));
	c.movaps(x86::xmm8, x86::oword_ptr(x86::rsp, 0x40));
	c.movaps(x86::xmm9, x86::oword_ptr(x86::rsp, 0x50));
	c.movaps(x86::xmm10, x86::oword_ptr(x86::rsp, 0x60));
	c.movaps(x86::xmm11, x86::oword_ptr(x86::rsp, 0x70));
	c.movaps(x86::xmm12, x86::oword_ptr(x86::rsp, 0x80));
	c.movaps(x86::xmm13, x86::oword_ptr(x86::rsp, 0x90));
	c.movaps(x86::xmm14, x86::oword_ptr(x86::rsp, 0xa0));
	c.movaps(x86::xmm15, x86::oword_ptr(x86::rsp, 0xb0));
	c.add(x86::rsp, 0xc8);
	c.pop(x86::rbx);
	c.pop(x86::rbp);
	c.pop(x86::rdi);
	c.pop(x86::rsi);
	c.pop(x86::r12);
	c.pop(x86::r13);
	c.pop(x86::r14);
	c.pop(x86::r15);
#else
	c.add(x86::rsp, +8);
	c.pop(x86::rbx);
	c.pop(x86::r12);
	c.pop(x86::r13);
	c.pop(x86::r14);
	c.pop(x86::r15);
	c.pop(x86::rbp);
#endif

	c.ret();
#elif defined(ARCH_ARM64)
	// See https://github.com/ghc/ghc/blob/master/rts/include/stg/MachRegs.h
	// for GHC calling convention definitions on Aarch64
	// and https://developer.arm.com/documentation/den0024/a/The-ABI-for-ARM-64-bit-Architecture/Register-use-in-the-AArch64-Procedure-Call-Standard/Parameters-in-general-purpose-registers
	// for AArch64 calling convention

	// PPU function argument layout:
	// x19 = m_exec
	// x20 = m_thread,
	// x21 = seg0
	// x22 = m_base
	// x23 - x25 = gpr[0] - gpr[3]

	// Push callee saved registers to the hv context
	// Assume our LLVM compiled code is unsafe and can clobber our stack. GHC on aarch64 treats stack as scratch.
	// We also want to store the register context at a fixed place so we can read the hypervisor state from any lcoation.
	// We need to save x18-x30 = 13 x 8B each + 8 bytes for 16B alignment = 112B

	// Pre-context save
	// Layout:
	// pc, sp
	// x18, x19...x30
	// NOTE: Do not touch x19..x30 before saving the registers!
	const u64 hv_register_array_offset = ::offset32(&ppu_thread::hv_ctx, &rpcs3::hypervisor_context_t::regs);
	Label hv_ctx_pc = c.newLabel(); // Used to hold the far jump return address

	// Sanity
	ensure(hv_register_array_offset < 4096); // Imm10

	c.mov(a64::x15, args[0]);
	c.add(a64::x14, a64::x15, Imm(hv_register_array_offset));  // Per-thread context save

	c.adr(a64::x15, hv_ctx_pc); // x15 = pc
	c.mov(a64::x13, a64::sp);   // x16 = sp

	c.stp(a64::x15, a64::x13, arm::Mem(a64::x14));
	c.stp(a64::x18, a64::x19, arm::Mem(a64::x14, 16));
	c.stp(a64::x20, a64::x21, arm::Mem(a64::x14, 32));
	c.stp(a64::x22, a64::x23, arm::Mem(a64::x14, 48));
	c.stp(a64::x24, a64::x25, arm::Mem(a64::x14, 64));
	c.stp(a64::x26, a64::x27, arm::Mem(a64::x14, 80));
	c.stp(a64::x28, a64::x29, arm::Mem(a64::x14, 96));
	c.str(a64::x30, arm::Mem(a64::x14, 112));

	// Load REG_Base - use absolute jump target to bypass rel jmp range limits
	c.mov(a64::x19, Imm(reinterpret_cast<u64>(&vm::g_exec_addr)));
	c.ldr(a64::x19, arm::Mem(a64::x19));
	// Load PPUThread struct base -> REG_Sp
	const arm::GpX ppu_t_base = a64::x20;
	c.mov(ppu_t_base, args[0]);
	// Load PC
	const arm::GpX pc = a64::x15;
	const arm::GpX cia_addr_reg = a64::x11;
	// Load offset value
	c.mov(cia_addr_reg, Imm(static_cast<u64>(::offset32(&ppu_thread::cia))));
	// Load cia
	c.ldr(pc.w(), arm::Mem(ppu_t_base, cia_addr_reg));

	// Multiply by 2 to index into ptr table
	c.add(pc, pc, pc);

	// Load call target
	const arm::GpX call_target = a64::x13;
	c.ldr(call_target, arm::Mem(a64::x19, pc));
	// Compute REG_Hp
	const arm::GpX reg_hp = a64::x21;
	c.mov(reg_hp, Imm(vm::g_exec_addr_seg_offset));
	c.add(reg_hp, reg_hp, pc, arm::Shift(arm::ShiftOp::kLSR, 2));
	c.ldrh(reg_hp.w(), arm::Mem(a64::x19, reg_hp));
	c.lsl(reg_hp.w(), reg_hp.w(), 13);

	// Load registers
	c.mov(a64::x22, Imm(reinterpret_cast<u64>(&vm::g_base_addr)));
	c.ldr(a64::x22, arm::Mem(a64::x22));

	const arm::GpX gpr_addr_reg = a64::x9;
	c.mov(gpr_addr_reg, Imm(static_cast<u64>(::offset32(&ppu_thread::gpr))));
	c.add(gpr_addr_reg, gpr_addr_reg, ppu_t_base);
	c.ldr(a64::x23, arm::Mem(gpr_addr_reg));
	c.ldr(a64::x24, arm::Mem(gpr_addr_reg, 8));
	c.ldr(a64::x25, arm::Mem(gpr_addr_reg, 16));

	// Thread context save. This is needed for PPU because different functions can switch between x19 and x20 for the base register.
	// We need a different solution to ensure that no matter which version, we get the right vaue on far return.
	c.mov(a64::x26, ppu_t_base);

	// Save thread pointer to stack. SP is the only register preserved across GHC calls.
	c.sub(a64::sp, a64::sp, Imm(16));
	c.str(a64::x20, arm::Mem(a64::sp));

	// GHC scratchpad mem. If managed correctly (i.e no returns ever), GHC functions should never require a stack frame.
	// We allocate a slab to use for all functions as they tail-call into each other.
	c.sub(a64::sp, a64::sp, Imm(8192));

	// Execute LLE call
	c.blr(call_target);

	// Return address after far jump. Reset sp and start unwinding...
	c.bind(hv_ctx_pc);

	// Clear scratchpad allocation
	c.add(a64::sp, a64::sp, Imm(8192));

	c.ldr(a64::x20, arm::Mem(a64::sp));
	c.add(a64::sp, a64::sp, Imm(16));

	// We either got here through normal "ret" which keeps our x20 intact, or we jumped here and the escape reset our x20 reg
	// Either way, x26 contains our thread base and we forcefully reset the stack pointer
	c.add(a64::x14, a64::x20, Imm(hv_register_array_offset));  // Per-thread context save

	c.ldr(a64::x15, arm::Mem(a64::x14, 8));
	c.ldp(a64::x18, a64::x19, arm::Mem(a64::x14, 16));
	c.ldp(a64::x20, a64::x21, arm::Mem(a64::x14, 32));
	c.ldp(a64::x22, a64::x23, arm::Mem(a64::x14, 48));
	c.ldp(a64::x24, a64::x25, arm::Mem(a64::x14, 64));
	c.ldp(a64::x26, a64::x27, arm::Mem(a64::x14, 80));
	c.ldp(a64::x28, a64::x29, arm::Mem(a64::x14, 96));
	c.ldr(a64::x30, arm::Mem(a64::x14, 112));

	// Return
	c.mov(a64::sp, a64::x15);
	c.ret(a64::x30);
#else
	// wasm32: no JIT — this assembly is unreachable.
	(void)c; (void)args;
#endif
});

const extern auto ppu_escape = build_function_asm<void(*)(ppu_thread*)>("ppu_escape", [](native_asm& c, auto& args)
{
	using namespace asmjit;

#if defined(ARCH_X64)
	// Restore native stack pointer (longjmp emulation)
	c.mov(x86::rsp, x86::qword_ptr(args[0], ::offset32(&ppu_thread::hv_ctx, &rpcs3::hypervisor_context_t::regs)));

	// Return to the return location
	c.sub(x86::rsp, 8);
	c.ret();
#elif defined(ARCH_ARM64)
	// We really shouldn't be using this, but an implementation shoudln't hurt
	// Far jump return. Only clobbers x30.
	const arm::GpX ppu_t_base = a64::x20;
	const u64 hv_register_array_offset = ::offset32(&ppu_thread::hv_ctx, &rpcs3::hypervisor_context_t::regs);
	c.mov(ppu_t_base, args[0]);
	c.mov(a64::x30, Imm(hv_register_array_offset));
	c.ldr(a64::x30, arm::Mem(ppu_t_base, a64::x30));
	c.ret(a64::x30);
#else
	// wasm32: no JIT — this assembly is unreachable. Suppress unused.
	(void)c; (void)args;
#endif
});

void ppu_recompiler_fallback(ppu_thread& ppu);

#if defined(ARCH_X64)
const auto ppu_recompiler_fallback_ghc = build_function_asm<void(*)(ppu_thread& ppu)>("", [](native_asm& c, auto& args)
{
	using namespace asmjit;

	c.mov(args[0], x86::rbp);
	c.jmp(ppu_recompiler_fallback);
});
#elif defined(ARCH_ARM64)
const auto ppu_recompiler_fallback_ghc = build_function_asm<void(*)(ppu_thread& ppu)>("", [](native_asm& c, auto& args)
{
	using namespace asmjit;

	Label fallback_fn = c.newLabel();
	Label escape_fn = c.newLabel();

	// This is called as GHC so the first arg is in x20.
	// Fix up the arg registers and call the real function.
	c.mov(args[0], a64::x20);
	c.ldr(a64::x13, arm::ptr(fallback_fn));
	c.blr(a64::x13);

	// There is no call-stack to return to in arm64 GHC. Escape to host.
	c.mov(a64::x0, a64::x20);
	c.ldr(a64::x13, arm::ptr(escape_fn));
	c.br(a64::x13);

	c.bind(fallback_fn);
	c.embedUInt64(reinterpret_cast<u64>(ppu_recompiler_fallback));
	c.bind(escape_fn);
	c.embedUInt64(reinterpret_cast<u64>(ppu_escape));
});
#else
const void* const ppu_recompiler_fallback_ghc = nullptr;
#endif

// Get pointer to executable cache
static inline u8* ppu_ptr(u32 addr)
{
	return vm::g_exec_addr + u64{addr} * 2;
}

static inline u8* ppu_seg_ptr(u32 addr)
{
	return vm::g_exec_addr + vm::g_exec_addr_seg_offset + (addr >> 1);
}

static inline ppu_intrp_func_t ppu_read(u32 addr)
{
	return read_from_ptr<ppu_intrp_func_t>(ppu_ptr(addr));
}

// Get interpreter cache value
static ppu_intrp_func_t ppu_cache(u32 addr)
{
	if (g_cfg.core.ppu_decoder != ppu_decoder_type::_static)
	{
		fmt::throw_exception("Invalid PPU decoder");
	}

	return g_fxo->get<ppu_interpreter_rt>().decode(vm::read32(addr));
}

static ppu_intrp_func ppu_ret = {[](ppu_thread& ppu, ppu_opcode_t, be_t<u32>* this_op, ppu_intrp_func*)
{
	// Fix PC and return (step execution)
	ppu.cia = vm::get_addr(this_op);
	return;
}};

static void ppu_fallback(ppu_thread& ppu, ppu_opcode_t op, be_t<u32>* this_op, ppu_intrp_func* next_fn)
{
	const auto _pc = vm::get_addr(this_op);
	const auto _fn = ppu_cache(_pc);
	write_to_ptr<ppu_intrp_func_t>(ppu_ptr(_pc), _fn);
	return _fn(ppu, op, this_op, next_fn);
}

// TODO: Make this a dispatch call
void ppu_recompiler_fallback(ppu_thread& ppu)
{
	perf_meter<"PPUFALL1"_u64> perf0;

	if (g_cfg.core.ppu_debug)
	{
		ppu_log.error("Unregistered PPU Function (LR=0x%x)", ppu.lr);
	}

	const auto& table = g_fxo->get<ppu_interpreter_rt>();

	while (true)
	{
		if (uptr func = uptr(ppu_read(ppu.cia)); func != reinterpret_cast<uptr>(ppu_recompiler_fallback_ghc))
		{
			// We found a recompiler function at cia, return
			break;
		}

		// Run one instruction in interpreter (TODO)
		const u32 op = vm::read32(ppu.cia);
		table.decode(op)(ppu, {op}, vm::_ptr<u32>(ppu.cia), &ppu_ret);

		if (ppu.test_stopped())
		{
			break;
		}
	}
}

void ppu_reservation_fallback(ppu_thread& ppu)
{
	perf_meter<"PPUFALL2"_u64> perf0;

	const auto& table = g_fxo->get<ppu_interpreter_rt>();

	while (true)
	{
		// Run one instruction in interpreter (TODO)
		const u32 op = vm::read32(ppu.cia);
		table.decode(op)(ppu, {op}, vm::_ptr<u32>(ppu.cia), &ppu_ret);

		if (!ppu.raddr || !ppu.use_full_rdata)
		{
			// We've escaped from reservation, return.
			return;
		}

		if (ppu.test_stopped())
		{
			return;
		}
	}
}

u32 ppu_read_mmio_aware_u32(u8* vm_base, u32 eal)
{
	if (eal >= RAW_SPU_BASE_ADDR)
	{
		// RawSPU MMIO
		auto thread = idm::get_unlocked<named_thread<spu_thread>>(spu_thread::find_raw_spu((eal - RAW_SPU_BASE_ADDR) / RAW_SPU_OFFSET));

		if (!thread)
		{
			// Access Violation
		}
		else if ((eal - RAW_SPU_BASE_ADDR) % RAW_SPU_OFFSET + sizeof(u32) - 1 < SPU_LS_SIZE) // LS access
		{
		}
		else if (u32 value{}; thread->read_reg(eal, value))
		{
			return std::bit_cast<be_t<u32>>(value);
		}
		else
		{
			fmt::throw_exception("Invalid RawSPU MMIO offset (addr=0x%x)", eal);
		}
	}

	// Value is assumed to be swapped
	return read_from_ptr<u32>(vm_base + eal);
}

void ppu_write_mmio_aware_u32(u8* vm_base, u32 eal, u32 value)
{
	if (eal >= RAW_SPU_BASE_ADDR)
	{
		// RawSPU MMIO
		auto thread = idm::get_unlocked<named_thread<spu_thread>>(spu_thread::find_raw_spu((eal - RAW_SPU_BASE_ADDR) / RAW_SPU_OFFSET));

		if (!thread)
		{
			// Access Violation
		}
		else if ((eal - RAW_SPU_BASE_ADDR) % RAW_SPU_OFFSET + sizeof(u32) - 1 < SPU_LS_SIZE) // LS access
		{
		}
		else if (thread->write_reg(eal, std::bit_cast<be_t<u32>>(value)))
		{
			return;
		}
		else
		{
			fmt::throw_exception("Invalid RawSPU MMIO offset (addr=0x%x)", eal);
		}
	}

	// Value is assumed swapped
	write_to_ptr<u32>(vm_base + eal, value);
}

extern bool ppu_test_address_may_be_mmio(std::span<const be_t<u32>> insts)
{
	std::set<u32> reg_offsets;
	bool found_raw_spu_base = false;
	bool found_spu_area_offset_element = false;

	for (u32 inst : insts)
	{
		// Common around MMIO (orders IO)
		if (inst == ppu_instructions::EIEIO())
		{
			return true;
		}

		const u32 op_imm16 = (inst & 0xfc00ffff);

		// RawSPU MMIO base
		// 0xe00000000 is a common constant so try to find an ORIS 0x10 or ADDIS 0x10 nearby (for multiplying SPU ID by it)
		if (op_imm16 == ppu_instructions::ADDIS({}, {}, -0x2000) || op_imm16 == ppu_instructions::ORIS({}, {}, 0xe000)  || op_imm16 == ppu_instructions::XORIS({}, {}, 0xe000))
		{
			found_raw_spu_base = true;

			if (found_spu_area_offset_element)
			{
				// Found both
				return true;
			}
		}
		else if (op_imm16 == ppu_instructions::ORIS({}, {}, 0x10) || op_imm16 == ppu_instructions::ADDIS({}, {}, 0x10))
		{
			found_spu_area_offset_element = true;

			if (found_raw_spu_base)
			{
				// Found both
				return true;
			}
		}
		// RawSPU MMIO base + problem state offset
		else if (op_imm16 == ppu_instructions::ADDIS({}, {}, -0x1ffc))
		{
			return true;
		}
		else if (op_imm16 == ppu_instructions::ORIS({}, {}, 0xe004))
		{
			return true;
		}
		else if (op_imm16 == ppu_instructions::XORIS({}, {}, 0xe004))
		{
			return true;
		}
		// RawSPU MMIO base + problem state offset + 64k of SNR1 offset
		else if (op_imm16 == ppu_instructions::ADDIS({}, {}, -0x1ffb))
		{
			return true;
		}
		else if (op_imm16 == ppu_instructions::ORIS({}, {}, 0xe005))
		{
			return true;
		}
		else if (op_imm16 == ppu_instructions::XORIS({}, {}, 0xe005))
		{
			return true;
		}
		// RawSPU MMIO base + problem state offset + 264k of SNR2 offset (STW allows 32K+- offset so in order to access SNR2 it needs to first add another 64k)
		// SNR2 is the only register currently implemented that has its 0x80000 bit is set so its the only one its hardcoded access is done this way
		else if (op_imm16 == ppu_instructions::ADDIS({}, {}, -0x1ffa))
		{
			return true;
		}
		else if (op_imm16 == ppu_instructions::ORIS({}, {}, 0xe006))
		{
			return true;
		}
		else if (op_imm16 == ppu_instructions::XORIS({}, {}, 0xe006))
		{
			return true;
		}
		// Try to detect a function that receives RawSPU problem state base pointer as an argument
		else if ((op_imm16 & ~0xffff) == ppu_instructions::LWZ({}, {}, 0) ||
			(op_imm16 & ~0xffff) == ppu_instructions::STW({}, {}, 0) ||
			(op_imm16 & ~0xffff) == ppu_instructions::ADDI({}, {}, 0))
		{
			const bool is_load = (op_imm16 & ~0xffff) == ppu_instructions::LWZ({}, {}, 0);
			const bool is_store = (op_imm16 & ~0xffff) == ppu_instructions::STW({}, {}, 0);
			const bool is_neither = !is_store && !is_load;
			const bool is_snr = (is_store || is_neither) && ((op_imm16 & 0xffff) == (SPU_RdSigNotify2_offs & 0xffff) || (op_imm16 & 0xffff) == (SPU_RdSigNotify1_offs & 0xffff));

			if (is_snr || spu_thread::test_is_problem_state_register_offset(op_imm16 & 0xffff, is_load || is_neither, is_store || is_neither))
			{
				reg_offsets.insert(op_imm16 & 0xffff);

				if (reg_offsets.size() >= 2)
				{
					// Assume high MMIO likelyhood if more than one offset appears in nearby code
					// Such as common IN_MBOX + OUT_MBOX
					return true;
				}
			}
		}
	}

	return false;
}

struct ppu_toc_manager
{
	std::unordered_map<u32, u32> toc_map;

	shared_mutex mutex;
};

static void ppu_check_toc(ppu_thread& ppu, ppu_opcode_t op, be_t<u32>* this_op, ppu_intrp_func* next_fn)
{
	ppu.cia = vm::get_addr(this_op);

	{
		auto& toc_manager = g_fxo->get<ppu_toc_manager>();

		reader_lock lock(toc_manager.mutex);

		auto& ppu_toc = toc_manager.toc_map;

		const auto found = ppu_toc.find(ppu.cia);

		if (found != ppu_toc.end())
		{
			const u32 toc = atomic_storage<u32>::load(found->second);

			// Compare TOC with expected value
			if (toc != umax && ppu.gpr[2] != toc)
			{
				ppu_log.error("Unexpected TOC (0x%x, expected 0x%x)", ppu.gpr[2], toc);
				atomic_storage<u32>::exchange(found->second, u32{umax});
			}
		}
	}

	// Fallback to the interpreter function
	return ppu_cache(ppu.cia)(ppu, op, this_op, next_fn);
}

extern void ppu_register_range(u32 addr, u32 size)
{
	if (!size)
	{
		ppu_log.error("ppu_register_range(0x%x): empty range", addr);
		return;
	}

	size = utils::align(size + addr % 0x10000, 0x10000);
	addr &= -0x10000;

#if defined(__EMSCRIPTEN__)
	// wasm32: validate the range fits in our smaller g_exec_addr region.
	// g_exec_addr_seg_offset = 64 MiB, total g_exec_addr = 128 MiB. Any
	// guest addr beyond ~32 MiB would have ppu_ptr (addr*2 = 64 MiB)
	// stomp on the seg ptr range. Skip out-of-range registrations.
	if (u64{addr} + u64{size} > 0x2000000ull) // 32 MiB cap
	{
		ensure(vm::page_protect(addr, size, 0, vm::page_executable));
		return;
	}
#endif

	// Register executable range at
	utils::memory_commit(ppu_ptr(addr), u64{size} * 2, utils::protection::rw);
	ensure(vm::page_protect(addr, size, 0, vm::page_executable));

	// Segment data
	utils::memory_commit(ppu_seg_ptr(addr), size >> 1, utils::protection::rw);

	if (g_cfg.core.ppu_debug)
	{
		utils::memory_commit(vm::g_stat_addr + addr, size);
	}

	const u64 seg_base = addr;

	while (size)
	{
		if (g_cfg.core.ppu_decoder == ppu_decoder_type::llvm)
		{
			// Assume addr is the start of first segment of PRX
			write_to_ptr<uptr>(ppu_ptr(addr), std::bit_cast<uptr>(ppu_recompiler_fallback_ghc));
			write_to_ptr<u16>(ppu_seg_ptr(addr), static_cast<u16>(seg_base >> 13));
		}
		else
		{
			write_to_ptr<ppu_intrp_func_t>(ppu_ptr(addr), ppu_fallback);
			write_to_ptr<u16>(ppu_seg_ptr(addr), 0);
		}

		addr += 4;
		size -= 4;
	}
}

static void ppu_far_jump(ppu_thread&, ppu_opcode_t, be_t<u32>*, ppu_intrp_func*);

extern void ppu_register_function_at(u32 addr, u32 size, ppu_intrp_func_t ptr = nullptr)
{
	// Initialize specific function
	if (ptr)
	{
		write_to_ptr<uptr>(ppu_ptr(addr), std::bit_cast<uptr>(ptr));
		return;
	}

	if (!size)
	{
		if (g_cfg.core.ppu_debug)
		{
			ppu_log.error("ppu_register_function_at(0x%x): empty range", addr);
		}

		return;
	}

	if (g_cfg.core.ppu_decoder == ppu_decoder_type::llvm)
	{
		return;
	}

	size = utils::align<u32>(size + addr % 4, 4);
	addr &= -4;

	// Initialize interpreter cache
	while (size)
	{
		if (auto old = ppu_read(addr); old != ppu_break && old != ppu_far_jump)
		{
			write_to_ptr<ppu_intrp_func_t>(ppu_ptr(addr), ppu_cache(addr));
		}

		addr += 4;
		size -= 4;
	}
}

extern void ppu_register_function_at(u32 addr, u32 size, u64 ptr)
{
	return ppu_register_function_at(addr, size, reinterpret_cast<ppu_intrp_func_t>(ptr));
}

u32 ppu_get_exported_func_addr(u32 fnid, const std::string& module_name);

void ppu_return_from_far_jump(ppu_thread& ppu, ppu_opcode_t, be_t<u32>*, ppu_intrp_func*)
{
	auto& calls_info = ppu.hle_func_calls_with_toc_info;
	ensure(!calls_info.empty());

	// Branch to next instruction after far jump call entry with restored R2 and LR
	const auto restore_info = &calls_info.back();
	ppu.cia = restore_info->cia + 4;
	ppu.lr = restore_info->saved_lr;
	ppu.gpr[2] = restore_info->saved_r2;

	calls_info.pop_back();
}

static const bool s_init_return_far_jump_func = []
{
	REG_HIDDEN_FUNC_PURE(ppu_return_from_far_jump);
	return true;
}();

struct ppu_far_jumps_t
{
	struct all_info_t
	{
		u32 target;
		bool link;
		bool with_toc;
		std::string module_name;
		ppu_intrp_func_t func;

		u32 get_target(u32 pc, ppu_thread* ppu = nullptr) const
		{
			u32 direct_target = this->target;

			bool to_link = this->link;
			bool from_opd = this->with_toc;

			if (!this->module_name.empty())
			{
				direct_target = ppu_get_exported_func_addr(direct_target, this->module_name);
			}

			if (from_opd && !vm::check_addr<sizeof(ppu_func_opd_t)>(direct_target))
			{
				// Avoid reading unmapped memory under mutex
				from_opd = false;
			}

			if (from_opd)
			{
				auto& opd = vm::_ref<ppu_func_opd_t>(direct_target);
				direct_target = opd.addr;

				// We modify LR to custom values here
				to_link = false;

				if (ppu)
				{
					auto& calls_info = ppu->hle_func_calls_with_toc_info;

					// Save LR and R2
					// Set LR to the this ppu_return_from_far_jump branch for restoration of registers
					// NOTE: In order to clean up this information all calls must return in order
					auto& saved_info = calls_info.emplace_back();
					saved_info.cia = pc;
					saved_info.saved_lr = std::exchange(ppu->lr, g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(ppu_return_from_far_jump), true));
					saved_info.saved_r2 = std::exchange(ppu->gpr[2], opd.rtoc);
				}
			}

			if (to_link && ppu)
			{
				ppu->lr = pc + 4;
			}

			return direct_target;
		}
	};

	ppu_far_jumps_t(int) noexcept {}

	std::map<u32, all_info_t> vals;
	std::pair<u32, u32> vals_range{0, 0};
	::jit_runtime rt;

	mutable shared_mutex mutex;

	void add_value(u32 addr, all_info_t info)
	{
		vals.insert_or_assign(addr, std::move(info));

		if (vals.size() == 1)
		{
			vals_range.first = addr;
			vals_range.second = addr;
		}
		else
		{
			vals_range.first = std::min<u32>(vals_range.first, addr);
			vals_range.second = std::max<u32>(vals_range.second, addr);
		}
	}

	// Get target address, 'ppu' is used in ppu_far_jump in order to modify registers
	u32 get_target(u32 pc, ppu_thread* ppu = nullptr)
	{
		if (vals_range.first > pc)
		{
			return 0;
		}

		if (vals_range.second < pc)
		{
			return 0;
		}

		reader_lock lock(mutex);

		if (auto it = vals.find(pc); it != vals.end())
		{
			all_info_t& all_info = it->second;
			return all_info.get_target(pc, ppu);
		}

		return 0;
	}

	// Get function patches in range (entry -> target)
	std::vector<std::pair<u32, u32>> get_targets(u32 pc, u32 size)
	{
		std::vector<std::pair<u32, u32>> targets;

		if (vals_range.first >= pc + size)
		{
			return targets;
		}

		if (vals_range.second < pc)
		{
			return targets;
		}

		reader_lock lock(mutex);

		auto it = vals.lower_bound(pc);

		if (it == vals.end())
		{
			return targets;
		}

		if (it->first >= pc + size)
		{
			return targets;
		}

		for (auto end = vals.lower_bound(pc + size); it != end; it++)
		{
			all_info_t& all_info = it->second;

			if (u32 target = all_info.get_target(it->first))
			{
				targets.emplace_back(it->first, target);
			}
		}

		return targets;
	}

	// Generate a mini-function which updates PC (for LLVM) and jumps to ppu_far_jump to handle redirections
	template <bool Locked = true>
	ppu_intrp_func_t gen_jump(u32 pc)
	{
		[[maybe_unused]] std::conditional_t<Locked, std::lock_guard<shared_mutex>, const shared_mutex&> lock(mutex);

		auto it = vals.find(pc);

		if (it == vals.end())
		{
			return nullptr;
		}

		if (!it->second.func)
		{
			it->second.func = build_function_asm<ppu_intrp_func_t>("", [&](native_asm& c, auto& args)
			{
				using namespace asmjit;

#ifdef ARCH_X64
				c.mov(args[0], x86::rbp);
				c.mov(args[2], vm::g_base_addr + pc);
				c.jmp(ppu_far_jump);
#elif defined(ARCH_ARM64)
				Label jmp_address = c.newLabel();
				Label this_op_address = c.newLabel();

				c.ldr(args[2], arm::ptr(this_op_address));
				c.ldr(args[1], arm::ptr(jmp_address));
				c.br(args[1]);

				c.align(AlignMode::kCode, 16);
				c.bind(jmp_address);
				c.embedUInt64(reinterpret_cast<u64>(ppu_far_jump));
				c.bind(this_op_address);
				c.embedUInt64(reinterpret_cast<u64>(vm::g_base_addr) + pc);
#else
				// wasm32: no JIT — suppress unused
				(void)c; (void)args; (void)pc;
#endif
			}, &rt);
		}

		return it->second.func;
	}
};

u32 ppu_get_far_jump(u32 pc)
{
	if (!g_fxo->is_init<ppu_far_jumps_t>())
	{
		return 0;
	}

	return g_fxo->get<ppu_far_jumps_t>().get_target(pc);
}

static void ppu_far_jump(ppu_thread& ppu, ppu_opcode_t, be_t<u32>* this_op, ppu_intrp_func*)
{
	const u32 cia = g_fxo->get<ppu_far_jumps_t>().get_target(vm::get_addr(this_op), &ppu);

	if (!vm::check_addr(cia, vm::page_executable))
	{
		fmt::throw_exception("PPU far jump failed! (returned cia = 0x%08x)", cia);
	}

	ppu.cia = cia;
}

bool ppu_form_branch_to_code(u32 entry, u32 target, bool link, bool with_toc, std::string module_name)
{
	// Force align entry and target
	entry &= -4;

	// Exported functions are using target as FNID, must not be changed
	if (module_name.empty())
	{
		target &= -4;

		u32 cia_target = target;

		if (with_toc)
		{
			ppu_func_opd_t opd{};
			if (!vm::try_access(target, &opd, sizeof(opd), false))
			{
				// Cannot access function descriptor
				return false;
			}

			// For now allow situations where OPD is changed later by patches or by the program itself
			//cia_target = opd.addr;

			// So force a valid target (executable, yet not equal to entry)
			cia_target = entry ^ 8;
		}

		// Target CIA must be aligned, executable and not equal with
		if (cia_target % 4 || entry == cia_target || !vm::check_addr(cia_target, vm::page_executable))
		{
			return false;
		}
	}

	// Entry must be executable
	if (!vm::check_addr(entry, vm::page_executable))
	{
		return false;
	}

	g_fxo->init<ppu_far_jumps_t>(0);

	if (!module_name.empty())
	{
		// Always use function descriptor for exported functions
		with_toc = true;
	}

	if (with_toc)
	{
		// Always link for calls with function descriptor
		link = true;
	}

	// Register branch target in host memory, not guest memory
	auto& jumps = g_fxo->get<ppu_far_jumps_t>();

	std::lock_guard lock(jumps.mutex);
	jumps.add_value(entry, ppu_far_jumps_t::all_info_t{target, link, with_toc, std::move(module_name)});
	ppu_register_function_at(entry, 4, g_cfg.core.ppu_decoder == ppu_decoder_type::_static ? &ppu_far_jump : ensure(g_fxo->get<ppu_far_jumps_t>().gen_jump<false>(entry)));

	return true;
}

bool ppu_form_branch_to_code(u32 entry, u32 target, bool link, bool with_toc)
{
	return ppu_form_branch_to_code(entry, target, link, with_toc, std::string{});
}

bool ppu_form_branch_to_code(u32 entry, u32 target, bool link)
{
	return ppu_form_branch_to_code(entry, target, link, false);
}

bool ppu_form_branch_to_code(u32 entry, u32 target)
{
	return ppu_form_branch_to_code(entry, target, false);
}

void ppu_remove_hle_instructions(u32 addr, u32 size)
{
	if (Emu.IsStopped() || !g_fxo->is_init<ppu_far_jumps_t>())
	{
		return;
	}

	auto& jumps = g_fxo->get<ppu_far_jumps_t>();

	std::lock_guard lock(jumps.mutex);

	for (auto it = jumps.vals.begin(); it != jumps.vals.end();)
	{
		if (it->first >= addr && it->first <= addr + size - 1 && size)
		{
			it = jumps.vals.erase(it);
			continue;
		}

		it++;
	}
}

atomic_t<bool> g_debugger_pause_all_threads_on_bp = false;

// Breakpoint entry point
static void ppu_break(ppu_thread& ppu, ppu_opcode_t, be_t<u32>* this_op, ppu_intrp_func* next_fn)
{
	const bool pause_all = g_debugger_pause_all_threads_on_bp;

	const u32 old_cia = vm::get_addr(this_op);
	ppu.cia = old_cia;

	// Pause
	ppu.state.atomic_op([&](bs_t<cpu_flag>& state)
	{
		if (pause_all) state += cpu_flag::dbg_global_pause;
		if (pause_all || !(state & cpu_flag::dbg_step)) state += cpu_flag::dbg_pause;
	});

	if (pause_all)
	{
		// Pause all other threads
		Emu.CallFromMainThread([]() { Emu.Pause(); });
	}

	if (ppu.check_state() || old_cia != atomic_storage<u32>::load(ppu.cia))
	{
		// Do not execute if PC changed
		return;
	}

	// Fallback to the interpreter function
	return ppu_cache(ppu.cia)(ppu, {*this_op}, this_op, ppu.state ? &ppu_ret : next_fn);
}

// Set or remove breakpoint
extern bool ppu_breakpoint(u32 addr, bool is_adding)
{
	if (addr % 4 || !vm::check_addr(addr, vm::page_executable) || g_cfg.core.ppu_decoder == ppu_decoder_type::llvm)
	{
		return false;
	}

	// Remove breakpoint parameters
	ppu_intrp_func_t func_original = 0;
	ppu_intrp_func_t breakpoint = &ppu_break;

	if (u32 hle_addr{}; g_fxo->is_init<ppu_function_manager>() && (hle_addr = g_fxo->get<ppu_function_manager>().addr))
	{
		// HLE function index
		const u32 index = (addr - hle_addr) / 8;

		if (addr % 8 == 4 && index < ppu_function_manager::get().size())
		{
			// HLE function placement
			func_original = ppu_function_manager::get()[index];
		}
	}

	if (!func_original)
	{
		// If not an HLE function use regular instruction function
		func_original = ppu_cache(addr);
	}

	if (is_adding)
	{
		if (ppu_read(addr) == ppu_fallback)
		{
			ppu_log.error("Unregistered instruction replaced with a breakpoint at 0x%08x", addr);
			func_original = ppu_fallback;
		}

		if (ppu_read(addr) != func_original)
		{
			return false;
		}

		write_to_ptr<ppu_intrp_func_t>(ppu_ptr(addr), breakpoint);
		return true;
	}

	if (ppu_read(addr) != breakpoint)
	{
		return false;
	}

	write_to_ptr<ppu_intrp_func_t>(ppu_ptr(addr), func_original);
	return true;
}

extern bool ppu_patch(u32 addr, u32 value)
{
	if (addr % 4)
	{
		ppu_log.fatal("Patch failed at 0x%x: unaligned memory address.", addr);
		return false;
	}

	vm::writer_lock rlock;

	if (!vm::check_addr(addr))
	{
		ppu_log.fatal("Patch failed at 0x%x: invalid memory address.", addr);
		return false;
	}

	const bool is_exec = vm::check_addr(addr, vm::page_executable);

	if (is_exec && g_cfg.core.ppu_decoder == ppu_decoder_type::llvm && !Emu.IsReady())
	{
		// TODO: support recompilers
		ppu_log.fatal("Patch failed at 0x%x: LLVM recompiler is used.", addr);
		return false;
	}

	*vm::get_super_ptr<u32>(addr) = value;

	if (is_exec)
	{
		if (auto old = ppu_read(addr); old != ppu_break && old != ppu_fallback)
		{
			write_to_ptr<ppu_intrp_func_t>(ppu_ptr(addr), ppu_cache(addr));
		}
	}

	return true;
}

std::array<u32, 2> op_branch_targets(u32 pc, ppu_opcode_t op)
{
	std::array<u32, 2> res{pc + 4, umax};

	if (u32 target = g_fxo->is_init<ppu_far_jumps_t>() ? g_fxo->get<ppu_far_jumps_t>().get_target(pc) : 0)
	{
		res[0] = target;
		return res;
	}

	switch (const auto type = g_ppu_itype.decode(op.opcode))
	{
	case ppu_itype::B:
	case ppu_itype::BC:
	{
		res[type == ppu_itype::BC ? 1 : 0] = ((op.aa ? 0 : pc) + (type == ppu_itype::B ? +op.bt24 : +op.bt14));
		break;
	}
	case ppu_itype::BCCTR:
	case ppu_itype::BCLR:
	case ppu_itype::UNK:
	{
		res[0] = umax;
		break;
	}
	default: break;
	}

	return res;
}

void ppu_thread::dump_regs(std::string& ret, std::any& custom_data) const
{
#if defined(__EMSCRIPTEN__)
	// wasm32: the native dump_regs body uses PPUDisAsm + std::any +
	// vm::check_addr<...> chains that trap `unreachable` here (the trap
	// site at the top of every emergency_exit-induced stack trace in the
	// browser console). Print only the bare essentials so emergency_exit
	// from any ensure()/throw_exception failure can complete its log
	// instead of killing the worker pthread with no diagnostic.
	(void)custom_data;
	fmt::append(ret, "cia=0x%08x lr=0x%08x r1=0x%llx (wasm32 stub)\n",
		static_cast<u32>(cia), static_cast<u32>(lr), static_cast<u64>(gpr[1]));
	return;
#else
	const system_state emu_state = Emu.GetStatus(false);
	const bool is_stopped_or_frozen = state & cpu_flag::exit || emu_state == system_state::frozen || emu_state <= system_state::stopping;
	const ppu_debugger_mode mode = debugger_mode.load();

	const bool is_decimal = !is_stopped_or_frozen && mode == ppu_debugger_mode::is_decimal;

	struct dump_registers_data_t
	{
		u32 preferred_cr_field_index = 7;
	};

	dump_registers_data_t* func_data = std::any_cast<dump_registers_data_t>(&custom_data);

	if (!func_data)
	{
		custom_data.reset();
		custom_data = std::make_any<dump_registers_data_t>();
		func_data = ensure(std::any_cast<dump_registers_data_t>(&custom_data));
	}

	PPUDisAsm dis_asm(cpu_disasm_mode::normal, vm::g_sudo_addr);

	for (uint i = 0; i < 32; ++i)
	{
		auto reg = gpr[i];

		// Fixup for syscall arguments
		if (current_function && i >= 3 && i <= 10) reg = syscall_args[i - 3];

		auto [is_const, const_value] = dis_asm.try_get_const_gpr_value(i, cia);

		if (const_value != reg)
		{
			// Expectation of predictable code path has not been met (such as a branch directly to the instruction)
			is_const = false;
		}

		fmt::append(ret, "r%d%s%s ", i, i <= 9 ? " " : "", is_const ? "©" : ":");

		bool printed_error = false;

		if ((reg >> 31) == 0x1'ffff'ffff)
		{
			const usz old_size = ret.size();

			fmt::append(ret, "%s (0x%x)", CellError{static_cast<u32>(reg)}, reg);

			// Test if failed to format (appended " 0x8".. in such case)
			if (ret[old_size] == '0')
			{
				// Failed
				ret.resize(old_size);
			}
			else
			{
				printed_error = true;
			}
		}

		if (!printed_error)
		{
			if (is_decimal)
			{
				fmt::append(ret, "%-11d", reg);
			}
			else
			{
				fmt::append(ret, "0x%-8llx", reg);
			}
		}

		constexpr u32 max_str_len = 32;
		constexpr u32 hex_count = 8;

		if (reg <= u32{umax} && vm::check_addr<max_str_len>(static_cast<u32>(reg)))
		{
			bool is_function = false;
			u32 toc = 0;

			auto is_exec_code = [&](u32 addr)
			{
				return addr % 4 == 0 && vm::check_addr(addr, vm::page_executable) && g_ppu_itype.decode(*vm::get_super_ptr<u32>(addr)) != ppu_itype::UNK;
			};

			if (const u32 reg_ptr = *vm::get_super_ptr<be_t<u32, 1>>(static_cast<u32>(reg));
				vm::check_addr<8>(reg_ptr) && !vm::check_addr(toc, vm::page_executable))
			{
				// Check executability and alignment
				if (reg % 4 == 0 && is_exec_code(reg_ptr))
				{
					toc = *vm::get_super_ptr<u32>(static_cast<u32>(reg + 4));

					if (toc % 4 == 0 && (toc >> 29) == (reg_ptr >> 29) && vm::check_addr(toc) && !vm::check_addr(toc, vm::page_executable))
					{
						is_function = true;
						reg = reg_ptr;
					}
				}
			}
			else if (is_exec_code(static_cast<u32>(reg)))
			{
				is_function = true;
			}

			const auto gpr_buf = vm::get_super_ptr<u8>(static_cast<u32>(reg));

			std::string buf_tmp(gpr_buf, gpr_buf + max_str_len);

			std::string_view sv(buf_tmp.data(), std::min<usz>(buf_tmp.size(), buf_tmp.find_first_of("\0\n"sv)));

			if (is_function)
			{
				if (toc)
				{
					fmt::append(ret, " -> func(at=0x%x, toc=0x%x)", reg, toc);
				}
				else
				{
					dis_asm.disasm(static_cast<u32>(reg));
					fmt::append(ret, " -> %s", dis_asm.last_opcode);
				}
			}
			// NTS: size of 3 and above is required
			// If ends with a newline, only one character is required
			else if ((sv.size() == buf_tmp.size() || (sv.size() >= (buf_tmp[sv.size()] == '\n' ? 1 : 3))) &&
				std::all_of(sv.begin(), sv.end(), [](u8 c){ return std::isprint(c); }))
			{
				fmt::append(ret, " -> \"%s\"", sv);
			}
			else
			{
				fmt::append(ret, " -> ");

				for (u32 j = 0; j < hex_count; ++j)
				{
					fmt::append(ret, "%02x ", buf_tmp[j]);
				}
			}
		}

		fmt::trim_back(ret);
		ret += '\n';
	}

	const u32 current_cia = cia;
	const u32 cr_packed = cr.pack();

	for (u32 addr :
	{
		current_cia,
		current_cia + 4,
		current_cia + 8,
		current_cia - 4,
		current_cia + 12,
	})
	{
		dis_asm.disasm(addr);

		if (dis_asm.last_opcode.size() <= 4)
		{
			continue;
		}

		usz index = dis_asm.last_opcode.rfind(",cr");

		if (index > dis_asm.last_opcode.size() - 4)
		{
			index = dis_asm.last_opcode.rfind(" cr");
		}

		if (index <= dis_asm.last_opcode.size() - 4)
		{
			const char result = dis_asm.last_opcode[index + 3];

			if (result >= '0' && result <= '7')
			{
				func_data->preferred_cr_field_index = result - '0';
				break;
			}
		}

		if (dis_asm.last_opcode.find("stdcx.") != umax || dis_asm.last_opcode.find("stwcx.") != umax)
		{
			// Modifying CR0
			func_data->preferred_cr_field_index = 0;
			break;
		}
	}

	const u32 displayed_cr_field = (cr_packed >> ((7 - func_data->preferred_cr_field_index) * 4)) & 0xf;

	fmt::append(ret, "CR: 0x%08x, CR%d: [LT=%u GT=%u EQ=%u SO=%u]\n", cr_packed, func_data->preferred_cr_field_index, displayed_cr_field >> 3, (displayed_cr_field >> 2) & 1, (displayed_cr_field >> 1) & 1, displayed_cr_field & 1);

	for (uint i = 0; i < 32; ++i)
	{
		const f64 r = fpr[i];

		if (!std::bit_cast<u64>(r))
		{
			fmt::append(ret, "f%d%s: %-12.6G [%-18s] (f32=0x%x)\n", i, i <= 9 ? " " : "", r, "", std::bit_cast<u32>(f32(r)));
			continue;
		}

		fmt::append(ret, "f%d%s: %-12.6G [0x%016x] (f32=0x%x)\n", i, i <= 9 ? " " : "", r, std::bit_cast<u64>(r), std::bit_cast<u32>(f32(r)));
	}

	for (uint i = 0; i < 32; ++i, ret += '\n')
	{
		fmt::append(ret, "v%d%s: ", i, i <= 9 ? " " : "");

		const auto r = vr[i];
		const u32 i3 = r.u32r[0];

		if (v128::from32p(i3) == r)
		{
			// Shortand formatting
			fmt::append(ret, "%08x", i3);
			fmt::append(ret, " [x: %g]", r.fr[0]);
		}
		else
		{
			fmt::append(ret, "%08x %08x %08x %08x", r.u32r[0], r.u32r[1], r.u32r[2], r.u32r[3]);
			fmt::append(ret, " [x: %g y: %g z: %g w: %g]", r.fr[0], r.fr[1], r.fr[2], r.fr[3]);
		}
	}

	fmt::append(ret, "CIA: 0x%x\n", current_cia);
	fmt::append(ret, "LR: 0x%llx\n", lr);
	fmt::append(ret, "CTR: 0x%llx\n", ctr);
	fmt::append(ret, "VRSAVE: 0x%08x\n", vrsave);
	fmt::append(ret, "XER: [CA=%u | OV=%u | SO=%u | CNT=%u]\n", xer.ca, xer.ov, xer.so, xer.cnt);
	fmt::append(ret, "VSCR: [SAT=%u | NJ=%u]\n", sat, nj);
	fmt::append(ret, "FPSCR: [FL=%u | FG=%u | FE=%u | FU=%u]\n", fpscr.fl, fpscr.fg, fpscr.fe, fpscr.fu);

	const u32 addr = raddr;

	if (addr)
		fmt::append(ret, "Reservation Addr: 0x%x", addr);
	else
		fmt::append(ret, "Reservation Addr: none");

	fmt::append(ret, "\nReservation Data (entire cache line):\n");

	be_t<u32> data[32]{};
	std::memcpy(data, rdata, sizeof(rdata)); // Show the data even if the reservation was lost inside the atomic loop

	if (addr && !use_full_rdata)
	{
		const u32 offset = addr & 0x78;

		fmt::append(ret, "[0x%02x] %08x %08x\n", offset, data[offset / sizeof(u32)], data[offset / sizeof(u32) + 1]);

		// Asterisk marks the offset of data that had been given to the guest PPU code
		*(&ret.back() - (addr & 4 ? 9 : 18)) = '*';
	}
	else
	{
		for (usz i = 0; i < std::size(data); i += 4)
		{
			fmt::append(ret, "[0x%02x] %08x %08x %08x %08x\n", i * sizeof(data[0])
				, data[i + 0], data[i + 1], data[i + 2], data[i + 3]);
		}

		if (addr)
		{
			// See the note above
			*(&ret.back() - (4 - (addr % 16 / 4)) * 9 - (8 - (addr % 128 / 16)) * std::size("[0x00]"sv)) = '*';
		}
	}
#endif // __EMSCRIPTEN__
}

std::string ppu_thread::dump_callstack() const
{
	std::string ret;

	fmt::append(ret, "Call stack:\n=========\n0x%08x (0x0) called\n", cia);

	for (const auto& sp : dump_callstack_list())
	{
		// TODO: function addresses too
		fmt::append(ret, "> from 0x%08x (sp=0x%08x)\n", sp.first, sp.second);
	}

	return ret;
}

std::vector<std::pair<u32, u32>> ppu_thread::dump_callstack_list() const
{
	//std::shared_lock rlock(vm::g_mutex); // Needs optimizations

	// Determine stack range
	const u64 r1 = gpr[1];

	if (r1 > u32{umax} || r1 % 0x10)
	{
		return {};
	}

	const u32 stack_ptr = static_cast<u32>(r1);

	if (!vm::check_addr(stack_ptr, vm::page_writable))
	{
		// Normally impossible unless the code does not follow ABI rules
		return {};
	}

	u32 stack_min = stack_ptr & ~0xfff;
	u32 stack_max = stack_min + 4096;

	while (stack_min && vm::check_addr(stack_min - 4096, vm::page_writable))
	{
		stack_min -= 4096;
	}

	while (stack_max + 4096 && vm::check_addr(stack_max, vm::page_writable))
	{
		stack_max += 4096;
	}

	std::vector<std::pair<u32, u32>> call_stack_list;

	bool is_first = true;
	bool skip_single_frame = false;

	const u64 _lr = this->lr;
	const u32 _cia = this->cia;
	const u64 gpr0 = this->gpr[0];

	for (
		u64 sp = r1;
		sp % 0x10 == 0u && sp >= stack_min && sp <= stack_max - ppu_stack_start_offset;
		is_first = false
		)
	{
		auto is_invalid = [](u64 addr)
		{
			if (addr > u32{umax} || addr % 4 || !vm::check_addr(static_cast<u32>(addr), vm::page_executable))
			{
				return true;
			}

			// Ignore HLE stop address
			return addr == g_fxo->get<ppu_function_manager>().func_addr(1, true);
		};

		if (is_first && !is_invalid(_lr))
		{
			// Detect functions with no stack or before LR has been stored

			// Tracking if instruction has already been passed through
			// Instead of using map or set, use two vectors relative to CIA and resize as needed
			std::vector<be_t<u32>> inst_neg;
			std::vector<be_t<u32>> inst_pos;

			auto get_inst = [&](u32 pos) -> be_t<u32>&
			{
				static be_t<u32> s_inst_empty{};

				if (pos < _cia)
				{
					const u32 neg_dist = (_cia - pos - 4) / 4;

					if (neg_dist >= inst_neg.size())
					{
						const u32 inst_bound = pos & -256;

						const usz old_size = inst_neg.size();
						const usz new_size = neg_dist + (pos - inst_bound) / 4 + 1;

						if (new_size >= 0x8000)
						{
							// Gross lower limit for the function (if it is that size it is unlikely that it is even a leaf function)
							return s_inst_empty;
						}

						inst_neg.resize(new_size);

						if (!vm::try_access(inst_bound, &inst_neg[old_size], ::narrow<u32>((new_size - old_size) * sizeof(be_t<u32>)), false))
						{
							// Failure (this would be detected as failure by zeroes)
						}

						// Reverse the array (because this buffer directs backwards in address)

						for (usz start = old_size, end = new_size - 1; start < end; start++, end--)
						{
							std::swap(inst_neg[start], inst_neg[end]);
						}
					}

					return inst_neg[neg_dist];
				}

				const u32 pos_dist = (pos - _cia) / 4;

				if (pos_dist >= inst_pos.size())
				{
					const u32 inst_bound = utils::align<u32>(pos, 256);

					const usz old_size = inst_pos.size();
					const usz new_size = pos_dist + (inst_bound - pos) / 4 + 1;

					if (new_size >= 0x8000)
					{
						// Gross upper limit for the function (if it is that size it is unlikely that it is even a leaf function)
						return s_inst_empty;
					}

					inst_pos.resize(new_size);

					if (!vm::try_access(pos, &inst_pos[old_size], ::narrow<u32>((new_size - old_size) * sizeof(be_t<u32>)), false))
					{
						// Failure (this would be detected as failure by zeroes)
					}
				}

				return inst_pos[pos_dist];
			};

			bool upper_abort = false;

			struct context_t
			{
				u32 start_point;
				bool maybe_leaf = false; // True if the function is leaf or at the very end/start of non-leaf
				bool non_leaf = false; // Absolutely not a leaf
				bool about_to_push_frame = false; // STDU incoming
				bool about_to_store_lr = false; // Link is about to be stored on stack
				bool about_to_pop_frame = false; // ADDI R1 is about to be issued
				bool about_to_load_link = false; // MTLR is about to be issued
				bool maybe_use_reg0_instead_of_lr = false; // Use R0 at the end of a non-leaf function if ADDI has been issued before MTLR
			};

			// Start with CIA
			std::deque<context_t> workload{context_t{_cia}};

			usz start = 0;

			for (; start < workload.size(); start++)
			{
				for (u32 wa = workload[start].start_point; vm::check_addr(wa, vm::page_executable);)
				{
					be_t<u32>& opcode = get_inst(wa);

					auto& [_, maybe_leaf, non_leaf, about_to_push_frame, about_to_store_lr,
						about_to_pop_frame, about_to_load_link, maybe_use_reg0_instead_of_lr] = workload[start];

					if (!opcode)
					{
						// Already passed or failure of reading
						break;
					}

					const ppu_opcode_t op{opcode};

					// Mark as passed through
					opcode = 0;

					const auto type = g_ppu_itype.decode(op.opcode);

					if (workload.size() == 1 && type == ppu_itype::STDU && op.rs == 1u && op.ra == 1u)
					{
						if (op.simm16 >= 0)
						{
							// Against ABI
							non_leaf = true;
							upper_abort = true;
							break;
						}

						// Saving LR to register: this is indeed a new function (ok because LR has not been saved yet)
						maybe_leaf = true;
						about_to_push_frame = true;
						about_to_pop_frame = false;
						upper_abort = true;
						break;
					}

					if (workload.size() == 1 && type == ppu_itype::STD && op.ra == 1u && op.rs == 0u)
					{
						bool found_matching_stdu = false;

						for (u32 back = 1; back < 20; back++)
						{
							be_t<u32>& opcode = get_inst(utils::sub_saturate<u32>(_cia, back * 4));

							if (!opcode)
							{
								// Already passed or failure of reading
								break;
							}

							const ppu_opcode_t test_op{opcode};

							const auto type = g_ppu_itype.decode(test_op.opcode);

							if (type == ppu_itype::BCLR)
							{
								break;
							}

							if (type == ppu_itype::STDU && test_op.rs == 1u && test_op.ra == 1u)
							{
								if (0 - (test_op.ds << 2) == (op.ds << 2) - 0x10)
								{
									found_matching_stdu = true;
								}

								break;
							}
						}

						if (found_matching_stdu)
						{
							// Saving LR to stack: this is indeed a new function (ok because LR has not been saved yet)
							maybe_leaf = true;
							about_to_store_lr = true;
							about_to_pop_frame = true;
							upper_abort = true;
							break;
						}
					}

					const u32 spr = ((op.spr >> 5) | ((op.spr & 0x1f) << 5));

					// It can be placed before or after STDU, ignore for now
					// if (workload.size() == 1 && type == ppu_itype::MFSPR && op.rs == 0u && spr == 0x8)
					// {
					// 	// Saving LR to register: this is indeed a new function (ok because LR has not been saved yet)
					// 	maybe_leaf = true;
					// 	about_to_store_lr = true;
					// 	about_to_pop_frame = true;
					// }

					if (type == ppu_itype::MTSPR && spr == 0x8 && op.rs == 0u)
					{
						// Test for special case: if ADDI R1 is not found later in code, it means that LR is not restored and R0 should be used instead
						// Can also search for ADDI R1 backwards and pull the value from stack (needs more research if it is more reliable)
						maybe_use_reg0_instead_of_lr = true;
					}

					if (type == ppu_itype::UNK)
					{
						// Ignore for now
						break;
					}

					if ((type & ppu_itype::branch) && op.lk)
					{
						// Gave up on LR before saving
						non_leaf = true;
						about_to_pop_frame = true;
						upper_abort = true;
						break;
					}

					// Even if BCLR is conditional, it still counts because LR value is ready for return
					if (type == ppu_itype::BCLR)
					{
						// Returned
						maybe_leaf = true;
						upper_abort = true;
						break;
					}

					if (type == ppu_itype::ADDI && op.ra == 1u && op.rd == 1u)
					{
						if (op.simm16 < 0)
						{
							// Against ABI
							non_leaf = true;
							upper_abort = true;
							break;
						}
						else if (op.simm16 > 0)
						{
							// Remember that SP is about to be restored
							about_to_pop_frame = true;
							non_leaf = true;
							upper_abort = true;
							break;
						}
					}

					const auto results = op_branch_targets(wa, op);

					bool proceeded = false;

					for (usz res_i = 0; res_i < results.size(); res_i++)
					{
						const u32 route_pc = results[res_i];

						if (route_pc == umax)
						{
							continue;
						}

						if (vm::check_addr(route_pc, vm::page_executable) && get_inst(route_pc))
						{
							if (proceeded)
							{
								// Remember next route start point
								workload.push_back(context_t{route_pc});
							}
							else
							{
								// Next PC
								wa = route_pc;
								proceeded = true;
							}
						}
					}
				}

				if (upper_abort)
				{
					break;
				}
			}

			const context_t& res = workload[std::min<usz>(start, workload.size() - 1)];

			if (res.maybe_leaf && !res.non_leaf)
			{
				const u32 result = res.maybe_use_reg0_instead_of_lr ? static_cast<u32>(gpr0) : static_cast<u32>(_lr);

				// Same stack as far as we know
				call_stack_list.emplace_back(result, static_cast<u32>(sp));

				if (res.about_to_store_lr)
				{
					// LR has yet to be stored on stack, ignore the stack value
					skip_single_frame = true;
				}
			}

			if (res.about_to_pop_frame || (res.maybe_leaf && !res.non_leaf))
			{
				const u64 temp_sp = *vm::get_super_ptr<u64>(static_cast<u32>(sp));

				if (temp_sp <= sp)
				{
					// Ensure inequality and that the old stack pointer is higher than current
					break;
				}

				// Read the first stack frame so caller addresses can be obtained
				sp = temp_sp;
				continue;
			}
		}

		u64 addr = *vm::get_super_ptr<u64>(static_cast<u32>(sp + 16));

		if (skip_single_frame)
		{
			skip_single_frame = false;
		}
		else if (!is_invalid(addr))
		{
			// TODO: function addresses too
			call_stack_list.emplace_back(static_cast<u32>(addr), static_cast<u32>(sp));
		}
		else if (!is_first)
		{
			break;
		}

		const u64 temp_sp = *vm::get_super_ptr<u64>(static_cast<u32>(sp));

		if (temp_sp <= sp)
		{
			// Ensure inequality and that the old stack pointer is higher than current
			break;
		}

		sp = temp_sp;

		is_first = false;
	}

	return call_stack_list;
}

void ppu_thread::dump_misc(std::string& ret, std::any& custom_data) const
{
	cpu_thread::dump_misc(ret, custom_data);

	if (ack_suspend)
	{
		if (ret.ends_with("\n"))
		{
			ret.pop_back();
		}

		fmt::append(ret, " (LV2 suspended)\n");
	}

	fmt::append(ret, "Priority: %d\n", prio.load().prio);
	fmt::append(ret, "Stack: 0x%x..0x%x\n", stack_addr, stack_addr + stack_size - 1);
	fmt::append(ret, "Joiner: %s\n", joiner.load());

	if (const auto size = cmd_queue.size())
		fmt::append(ret, "Commands: %u\n", size);

	const char* _func = current_function;

	if (_func)
	{
		ret += "In function: ";
		ret += _func;
		ret += '\n';

		for (u32 i = 3; i <= 10; i++)
			if (u64 v = gpr[i]; v != syscall_args[i - 3])
				fmt::append(ret, " ** r%d: 0x%llx\n", i, v);
	}
	else if (is_paused() || is_stopped())
	{
		if (const auto last_func = last_function)
		{
			_func = last_func;
			ret += "Last function: ";
			ret += _func;
			ret += '\n';
		}
	}

	if (const auto _time = start_time)
	{
		fmt::append(ret, "Waiting: %fs\n", (get_guest_system_time() - _time) / 1000000.);
	}
	else
	{
		ret += '\n';
	}

	if (!_func)
	{
		ret += '\n';
	}
}

void ppu_thread::dump_all(std::string& ret) const
{
	cpu_thread::dump_all(ret);

	if (call_history.data.size() > 1)
	{
		ret +=
			"\nCalling History:"
			"\n================";

		fmt::append(ret, "%s", call_history);
	}

	if (syscall_history.data.size() > 1)
	{
		ret +=
			"\nHLE/LV2 History:"
			"\n================";

		fmt::append(ret, "%s", syscall_history);
	}
}

extern thread_local std::string(*g_tls_log_prefix)();

void ppu_thread::cpu_task()
{
	std::fesetround(FE_TONEAREST);

	if (g_cfg.core.set_daz_and_ftz)
	{
		gv_set_zeroing_denormals();
	}
	else
	{
		gv_unset_zeroing_denormals();
	}

	// Execute cmd_queue
	while (cmd64 cmd = cmd_wait())
	{
		const u32 arg = cmd.arg2<u32>(); // 32-bit arg extracted

// dispatch logging removed for perf — see exec_task heartbeat instead

		switch (auto type = cmd.arg1<ppu_cmd>())
		{
		case ppu_cmd::opcode:
		{
			cmd_pop(), g_fxo->get<ppu_interpreter_rt>().decode(arg)(*this, {arg}, vm::_ptr<u32>(cia - 4), &ppu_ret);
			break;
		}
		case ppu_cmd::set_gpr:
		{
			if (arg >= 32)
			{
				fmt::throw_exception("Invalid ppu_cmd::set_gpr arg (0x%x)", arg);
			}

			gpr[arg % 32] = cmd_get(1).as<u64>();
			cmd_pop(1);
			break;
		}
		case ppu_cmd::set_args:
		{
			if (arg > 8)
			{
				fmt::throw_exception("Unsupported ppu_cmd::set_args size (0x%x)", arg);
			}

			for (u32 i = 0; i < arg; i++)
			{
				gpr[i + 3] = cmd_get(1 + i).as<u64>();
			}

			cmd_pop(arg);
			break;
		}
		case ppu_cmd::lle_call:
		{
#ifdef __APPLE__
			pthread_jit_write_protect_np(true);
#endif
			const vm::ptr<u32> opd(arg < 32 ? vm::cast(gpr[arg]) : vm::cast(arg));
			cmd_pop(), fast_call(opd[0], opd[1]);
			break;
		}
		case ppu_cmd::entry_call:
		{
#ifdef __APPLE__
			pthread_jit_write_protect_np(true);
#endif
			cmd_pop(), fast_call(entry_func.addr, entry_func.rtoc, true);
			break;
		}
		case ppu_cmd::hle_call:
		{
			cmd_pop(), ::at32(ppu_function_manager::get(), arg)(*this, {arg}, vm::_ptr<u32>(cia - 4), &ppu_ret);
			break;
		}
		case ppu_cmd::opd_call:
		{
#ifdef __APPLE__
			pthread_jit_write_protect_np(true);
#endif
			const ppu_func_opd_t opd = cmd_get(1).as<ppu_func_opd_t>();
			cmd_pop(1), fast_call(opd.addr, opd.rtoc);
			break;
		}
		case ppu_cmd::ptr_call:
		{
			const ppu_intrp_func_t func = cmd_get(1).as<ppu_intrp_func_t>();
			cmd_pop(1), func(*this, {}, vm::_ptr<u32>(cia - 4), &ppu_ret);
			break;
		}
		case ppu_cmd::cia_call:
		{
			loaded_from_savestate = true;
			cmd_pop(), fast_call(std::exchange(cia, 0), gpr[2], true);
			break;
		}
		case ppu_cmd::initialize:
		{
#ifdef __APPLE__
			pthread_jit_write_protect_np(false);
#endif
			cmd_pop();

			ppu_initialize();

			if (Emu.IsStopped())
			{
				return;
			}

			spu_cache::initialize();

#ifdef __APPLE__
			pthread_jit_write_protect_np(true);
#endif
#ifdef ARCH_ARM64
			// Flush all cache lines after potentially writing executable code
			asm("ISB");
			asm("DSB ISH");
#endif

			// Wait until the progress dialog is closed.
			// We don't want to open a cell dialog while a native progress dialog is still open.
			while (u32 v = g_progr_ptotal)
			{
				if (Emu.IsStopped())
				{
					return;
				}

				g_progr_ptotal.wait(v);
			}

			g_fxo->get<progress_dialog_workaround>().show_overlay_message_only = true;

			// Sadly we can't postpone initializing guest time because we need to run PPU threads
			// (the farther it's postponed, the less accuracy of guest time has been lost)
			Emu.FixGuestTime();

			// Run SPUs waiting on a syscall (savestates related)
			idm::select<named_thread<spu_thread>>([&](u32, named_thread<spu_thread>& spu)
			{
				if (spu.group && spu.index == spu.group->waiter_spu_index)
				{
					if (std::exchange(spu.stop_flag_removal_protection, false))
					{
						return;
					}

					ensure(spu.state.test_and_reset(cpu_flag::stop));
					spu.state.notify_one();
				}
			});

			// Check if this is the only PPU left to initialize (savestates related)
			if (lv2_obj::is_scheduler_ready())
			{
				if (Emu.IsStarting())
				{
					Emu.FinalizeRunRequest();
				}
			}

			break;
		}
		case ppu_cmd::sleep:
		{
			cmd_pop(), lv2_obj::sleep(*this);
			break;
		}
		case ppu_cmd::reset_stack:
		{
			cmd_pop(), gpr[1] = stack_addr + stack_size - ppu_stack_start_offset;
			break;
		}
		default:
		{
			fmt::throw_exception("Unknown ppu_cmd(0x%x)", static_cast<u32>(type));
		}
		}
	}
}

void ppu_thread::cpu_sleep()
{
	// Clear reservation
	raddr = 0;

	// Setup wait flag and memory flags to relock itself
	state += cpu_flag::wait + cpu_flag::memory;

	if (auto ptr = vm::g_tls_locked)
	{
		ptr->compare_and_swap(this, nullptr);
	}

	lv2_obj::awake(this);
}

void ppu_thread::cpu_on_stop()
{
	if (current_function && is_stopped())
	{
		if (start_time)
		{
			ppu_log.warning("'%s' aborted (%fs)", current_function, (get_guest_system_time() - start_time) / 1000000.);
		}
		else
		{
			ppu_log.warning("'%s' aborted", current_function);
		}
	}

	current_function = {};

	// TODO: More conditions
	if (Emu.IsStopped() && g_cfg.core.ppu_debug)
	{
		std::string ret;
		dump_all(ret);
		ppu_log.notice("thread context: %s", ret);
	}

	if (is_stopped())
	{
		if (last_succ == 0 && last_fail == 0 && exec_bytes == 0)
		{
			perf_log.notice("PPU thread perf stats are not available.");
		}
		else
		{
			perf_log.notice("Perf stats for STCX reload: success %u, failure %u", last_succ, last_fail);
			perf_log.notice("Perf stats for instructions: total %u", exec_bytes / 4);
		}
	}
}

void ppu_thread::cpu_wait(bs_t<cpu_flag> old)
{
	// Meanwhile while waiting, notify SPU waiters
	if (u32 addr = res_notify)
	{
		res_notify = 0;
		res_notify_postpone_streak = 0;

		if (res_notify_time + 128 == (vm::reservation_acquire(addr) & -128))
		{
			vm::reservation_notifier_notify(addr, res_notify_time);
		}
	}

	if (old != state)
	{
		return;
	}

	state.wait(old);
}

void ppu_thread::exec_task()
{
	if (g_cfg.core.ppu_decoder != ppu_decoder_type::_static)
	{
		// HVContext push to allow recursion. This happens with guest callback invocations.
		const auto old_hv_ctx = hv_ctx;

		while (true)
		{
			if (state) [[unlikely]]
			{
				if (check_state())
					break;
			}

			ppu_gateway(this);
		}

		// HVContext pop
		hv_ctx = old_hv_ctx;
		return;
	}

	const auto cache = vm::g_exec_addr;
	const auto mem_ = vm::g_base_addr;
#if defined(__EMSCRIPTEN__)
	// Snapshot the EBOOT TOC + code range once (populated in ppu_load_exec
	// before any PPU thread starts) so the null-TOC cascade fix in the hot
	// loop is a couple of register compares, not per-instruction atomic loads.
	const u32 eboot_toc = g_eboot_toc.load();
	const u32 eboot_beg = g_eboot_code_beg.load();
	const u32 eboot_end = g_eboot_code_end.load();
	// Cheap heartbeat: emit "alive" every 5M instructions. Lets us see
	// when a PPU thread is stuck in a tight loop vs actually progressing
	// through code, without taxing the hot path (one printf per ~10s of
	// interpreter work on wasm32 ≈ 0 % overhead).
	u32 s_hb = 0;
	// First-instruction guard: log the first dispatch + sanity-check that
	// the interpreter cache slot is non-null. Without this guard, an
	// unmapped cia (e.g. HLE_RETURN where the cache wasn't populated)
	// dispatches a null function pointer → wasm `unreachable` trap that
	// kills the worker with no diagnostic. Logs once per thread.
	bool s_first = true;
#endif

	while (true)
	{
		if (test_stopped()) [[unlikely]]
		{
			return;
		}

		gv_zeroupper();

#if defined(__EMSCRIPTEN__)
		++s_hb;
		// (heartbeat removed — s_hb resets on every block/preempt, so it rarely
		// reached the threshold; using the lv2 sync-channel trace logs instead.)

		// MAINCIA: the vblank gate is fixed (RenderLoop loops ready), but the
		// game loop (main_thread, id 0x1000000) went silent after MAIN-RET and
		// produces no frame -> no flip. A thread_local counter (persists across
		// blocks, unlike s_hb) samples main_thread's cia: if it fires with a
		// varying cia main is grinding game logic (slow); if it clusters at one
		// cia main is busy-spinning there (find the poll); if it never fires
		// main is blocked in a wait. ~1 line/sec while main executes.
		// WORKERCIA: sample the EE loader threads (ResourceLoop/WorkerThread/etc.,
		// ids 0x1000002..0x100000c) to see where they block/spin after their
		// trophy/savedata/userinfo init — the asset loader never reads data.vfs,
		// so resources stay unregistered and main's poll never matches.
		if ((this->id & 0xFF000000u) == 0x01000000u) // PROGRESS heartbeat: sample main + ResourceLoop so the user can see the boot advancing toward the flip (non-headless is stable, so light printf is fine)
		{
			if (this->id == 0x1000000u) { static thread_local u32 s_mf = 0; if ((++s_mf & 0x3FFFFu) == 0u) printf("[MAINFINE] cia=0x%x lr=0x%x r3=0x%x\n", cia, static_cast<u32>(lr), static_cast<u32>(gpr[3])); }
			// REGTRACK (2026-06-03): track the resource-registration COUNT over the whole run, independent of
			// main's polling. count @ (*(0x59fb9c)) + 0x290000 - 0x6f9c. Does it climb past main's targets
			// (#587-595 = 0x24b/0x251-0x253) -> slowness, or plateau -> a real registration stall?
			if (this->id == 0x1000000u) { static thread_local u32 s_rt = 0; if ((++s_rt & 0x1FFFFu) == 0u) { const u32 base = reinterpret_cast<be_t<u32>*>(mem_ + 0x59fb9cull)->value(); if (base >= 0x400000u && base < 0xc00000u) { const u32 cntv = reinterpret_cast<be_t<u32>*>(mem_ + (u64{base} + 0x290000u - 0x6f9cu))->value(); printf("[REGTRACK] cnt=%u base=0x%x\n", cntv, base); } } }
			// REGSETUP (2026-06-03): is the register-setup fn 0x86a40 EVER called with a DYNAMIC id (r4>=0x200,
			// e.g. main's targets 0x251-0x253) or only static manifest ids (<=0x15f)? Catches a gated dynamic/
			// second-manifest register path (=> find why it fails) vs none-at-all (=> dynamic reg never coded/reached).
			if (cia == 0x86a40u && this->id == 0x1000000u) { static thread_local u32 s_rs = 0; const u32 n = ++s_rs; const u32 r3v = static_cast<u32>(gpr[3]); const u32 r1v = static_cast<u32>(gpr[1]); const bool danger = (r3v >= 0x50080640u && r3v <= 0x500806d0u); if (n <= 8u || danger || (n & 0xFFFu) == 0u) printf("[REGSETUP] #%u id=0x%x r1=0x%x r3=0x%x lr=0x%x%s\n", n, static_cast<u32>(gpr[4]), r1v, r3v, static_cast<u32>(lr), danger ? " <==DANGER(hits saved-LR slot)" : ""); }
			if (cia == 0x1d44d0u && this->id == 0x1000000u) { static thread_local u32 s_rf = 0; const u32 n = ++s_rf; if (n <= 40u) printf("[REGFN-ENTRY] #%u r1=0x%x lr=0x%x r3=0x%x\n", n, static_cast<u32>(gpr[1]), static_cast<u32>(lr), static_cast<u32>(gpr[3])); }
			// SESSION 9 RESOLUTION (2026-06-04): the 0x86a40 jump-table dispatch @0x86b90 is CORRECT in the
			// interpreter — rldic computes r10=0x39c and bctr branches to 0x8b5cc, verified live via E7RLDIC/
			// E7BCTR with STUCKSKIP disabled. The 9-session "mis-dispatch to LR (0x1d4798)" was NOT an
			// interpreter bug: it was the STUCKSKIP band-aid (below, now disabled) hijacking cia=0x86b90 -> lr
			// after 800 same-r3 reps — which triggered on LEGITIMATE repeated registration calls and thereby
			// CAUSED the very null-loop it was meant to skip. With STUCKSKIP gone the dispatch runs natively and
			// deterministically (flakiness also gone). The JTFIX workaround below is retired (disabled); it only
			// ever "worked" by running before STUCKSKIP and continuing to the correct target.
			if (false && cia == 0x86b90u && this->id == 0x1000000u) [[unlikely]] {  // JTFIX retired — dispatch is correct natively
				const u32 r4v = static_cast<u32>(gpr[4]);
				const u32 r9v = reinterpret_cast<be_t<u32>*>(mem_ + (u64{static_cast<u32>(gpr[2])} - 0x6e1cu))->value();
				const u32 idx = r4v << 2;
				const u32 entry = reinterpret_cast<be_t<u32>*>(mem_ + (u64{r9v} + u64{idx}))->value();
				const u32 target = r9v + static_cast<u32>(static_cast<s32>(entry));
				if (target >= 0x10000u && target < 0xc00000u) {
					gpr[9] = r9v; gpr[10] = idx; gpr[11] = entry; gpr[5] = static_cast<u64>(static_cast<s64>(static_cast<s32>(entry))); gpr[4] = target;
					static thread_local u32 s_jt = 0; if (s_jt++ < 10u) printf("[JTFIX] id=0x%x r2=0x%x eboot_toc=0x%x %s -> 0x%x\n", r4v, static_cast<u32>(gpr[2]), eboot_toc, (static_cast<u32>(gpr[2]) == eboot_toc) ? "(r2==eboot_toc)" : "(r2 DIFFERS!)", target);
					cia = target; continue;
				}
			}
			{ static thread_local bool s_arm = false; static thread_local u32 s_prev = 0, s_pcia = 0, s_cnt = 0; if (this->id == 0x1000000u) { if (!s_arm && cia == 0x86a40u && static_cast<u32>(lr) == 0x1d4798u && static_cast<u32>(gpr[4]) == 0xe7u) { s_arm = true; { extern bool g_e7mon; g_e7mon = true; } s_prev = static_cast<u32>(gpr[1]); s_pcia = cia; printf("[R1MON] ARMED id=0xe7 0x86a40 entry r1=0x%x\n", s_prev); } if (s_arm) { const u32 r1n = static_cast<u32>(gpr[1]); { static thread_local u32 s_blk = 0; if (s_blk++ < 40u) printf("[BLK] @0x%x r1=0x%x\n", cia, r1n); if (cia == 0x86b90u) { const u32 r4v = static_cast<u32>(gpr[4]); const u32 r2v = static_cast<u32>(gpr[2]); const u32 r9v = reinterpret_cast<be_t<u32>*>(mem_ + (u64{r2v} - 0x6e1cu))->value(); const u32 entry = reinterpret_cast<be_t<u32>*>(mem_ + (u64{r9v} + (u64{r4v} << 2)))->value(); const u32 tgt = r9v + static_cast<u32>(static_cast<s32>(entry)); printf("[DISPATCH] r4=0x%x r9base=0x%x entry[r4*4]=0x%x correct_target=0x%x (bctr actually went to 0x1d4798)\n", r4v, r9v, entry, tgt); const u64 r4full = gpr[4]; const u64 rot = (r4full << 2) | (r4full >> 62); const u64 our_r10 = rot & 0x3FFFFFFFCull; const u32 oe = reinterpret_cast<be_t<u32>*>(mem_ + (u64{r9v} + static_cast<u32>(our_r10)))->value(); const u32 ot = r9v + static_cast<u32>(static_cast<s32>(oe)); printf("[DISP2] r4FULL=0x%llx rldic_r10=0x%llx (want 0x39c) entry=0x%x our_target=0x%x\n", static_cast<unsigned long long>(r4full), static_cast<unsigned long long>(our_r10), oe, ot); } } if (r1n != s_prev) { if (s_cnt++ < 120u) printf("[R1CHG] instr@0x%x changed r1 0x%x -> 0x%x (%+d); now @0x%x\n", s_pcia, s_prev, r1n, static_cast<int>(static_cast<s32>(r1n) - static_cast<s32>(s_prev)), cia); s_prev = r1n; } s_pcia = cia; if (cia == 0x1d4798u) { printf("[R1MON] DISARM @0x1d4798 r1=0x%x %s\n", r1n, (r1n != 0x50080600u) ? "<==LEAKED" : "ok"); { extern bool g_e7mon; g_e7mon = false; } s_arm = false; } } } }
			if (this->id == 0x1000000u && cia == 0x86a40u && static_cast<u32>(lr) == 0x1d4798u) { static thread_local u32 s_t0 = 0; const u32 n0 = ++s_t0; if (n0 <= 80u || static_cast<u32>(gpr[1]) != 0x50080600u) printf("[R1TRACE] A #%u 0x86a40-entry r1=0x%x%s\n", n0, static_cast<u32>(gpr[1]), static_cast<u32>(gpr[1])!=0x50080600u?" <==WRONG":""); }
			if (this->id == 0x1000000u && cia == 0x928bcu) { static thread_local u32 s_t1 = 0; if (false) printf("[R1TRACE] B 0x928bc-before-poll r1=0x%x lr=0x%x\n", static_cast<u32>(gpr[1]), static_cast<u32>(lr)); }
			if (this->id == 0x1000000u && cia == 0x928c0u) { static thread_local u32 s_t2 = 0; if (false) printf("[R1TRACE] C 0x928c0-after-poll r1=0x%x lr=0x%x\n", static_cast<u32>(gpr[1]), static_cast<u32>(lr)); }
			if (this->id == 0x1000000u && cia == 0x1d4798u) { static thread_local u32 s_t3 = 0; const u32 n3 = ++s_t3; if (n3 <= 80u || static_cast<u32>(gpr[1]) != 0x50080600u) printf("[R1TRACE] D #%u 0x1d4798-returned r1=0x%x%s\n", n3, static_cast<u32>(gpr[1]), static_cast<u32>(gpr[1])!=0x50080600u?" <==WRONG(leaked)":""); }
			if (this->id == 0x1000000u && cia == 0x2761cu && static_cast<u32>(lr) == 0x928c0u) { static thread_local u32 s_pi = 0; if (s_pi++ < 24u) printf("[POLL-IN] r1=0x%x lr=0x%x (reg-path poll entry)\n", static_cast<u32>(gpr[1]), static_cast<u32>(lr)); }
			if (this->id == 0x1000000u && cia == 0x2761cu && static_cast<u32>(lr) == 0x928c0u) { static thread_local bool s_ss = false; if (!s_ss) { s_ss = true; const u32 sp = static_cast<u32>(gpr[1]); printf("[STKSCAN] reg-path poll r1=0x%x — stack return-addrs (frame chain):\n", sp); for (u32 a = sp; a < sp + 0x360u; a += 8u) { const u32 v = reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); if (v >= 0x10000u && v < 0x600000u) printf("  [STK] @0x%x = 0x%x\n", a, v); } } }
			if (this->id == 0x1000000u && cia == 0x276fcu && static_cast<u32>(lr) == 0x928c0u) { static thread_local u32 s_po = 0; if (s_po++ < 24u) printf("[POLL-OUT] r1=0x%x lr=0x%x (reg-path poll blr; should equal POLL-IN)\n", static_cast<u32>(gpr[1]), static_cast<u32>(lr)); }
			if (this->id == 0x1000000u && (cia == 0x1d4954u || cia == 0x1d4958u || cia == 0x2f2ecu)) { static thread_local u32 s_xe = 0; if (s_xe++ < 16u) printf("[REGFN-EXIT] cia=0x%x r1=0x%x lr=0x%x  (0x1d4954=after 0x32dd0 leaf; 0x1d4958=epilogue after 0x2f2ec; 0x2f2ec=entry)\n", cia, static_cast<u32>(gpr[1]), static_cast<u32>(lr)); }
			if (this->id == 0x1000000u && static_cast<u32>(gpr[1]) == 0x50080590u && cia >= 0x1d47a0u && cia <= 0x1d4958u) { static thread_local bool s_rl = false; if (!s_rl) { s_rl = true; printf("[R1LOW] first r1=0x50080590 in REGFN at cia=0x%x lr=0x%x (the call returning here dropped r1 by 0x70)\n", cia, static_cast<u32>(lr)); } }
			// REGGUARD (2026-06-03): at the commit guard 0x69070, log the resource id (obj+0x1250) and the
			// guard values (obj+0x1150, obj+0x11d0). Hypothesis: main's targets (rid>=0x200) have a1150==0
			// (data attributes never populated by the loader) -> commit SKIPPED -> not in index -> main waits.
			if (cia == 0x69070u) { const u32 ob = static_cast<u32>(gpr[11]); const auto rh = [mem_](u32 a){ return reinterpret_cast<be_t<u16>*>(mem_ + u64{a})->value(); }; const u32 rid = rh(ob + 0x1250u); static thread_local u32 s_gg = 0; const u32 n = ++s_gg; if (n <= 6u || rid >= 0x200u || (n & 0x7FFu) == 0u) printf("[REGGUARD] #%u rid=0x%x a1150=0x%x a11d0=0x%x obj=0x%x\n", n, rid, rh(ob + 0x1150u), rh(ob + 0x11d0u), ob); }
			// LDRFINE (2026-06-03): map the loader's (0x1000006) PROCESS loop — does it ever reach register
			// code (0x86a40/0x69xxx/0xe0xxx) after read+inflate, or is it purely I/O (so registration is main's
			// job, gated)? Dense sample of its live PC.
			if (this->id == 0x1000006u) { static thread_local u32 s_lf = 0; if ((++s_lf & 0x3FFFu) == 0u) printf("[LDRFINE] cia=0x%x lr=0x%x r3=0x%x\n", static_cast<u32>(cia), static_cast<u32>(lr), static_cast<u32>(gpr[3])); }
			// LOGOSCAN (2026-06-03): one-shot post-boot — scan emu memory for logo.gtf's decompressed pixel
			// SIGNATURE (32 ARGB bytes from the Terraria-logo leaves, extracted offline from data.vfs). If
			// found, the game DID load+decompress the logo (so P1 is registration-only + I can render the
			// game's OWN loaded copy via P2). If absent, the boot gates BEFORE loading the logos.
			if (this->id == 0x1000000u) { static thread_local u32 s_gs = 0; if (++s_gs == 2500000u) {
				// two signatures of offline-Python-decompressed data.vfs chunks:
				//  sigL = logo.gtf pixels (a VISUAL asset; tests whether boot visuals load at all)
				//  sigA = a loaded .msf AUDIO chunk (#512, KNOWN-loaded; tests INFLATE bit-correctness)
				static const u8 sigL[32] = {0xff,0x0c,0x61,0x23,0xff,0x22,0xa8,0x51,0xff,0x22,0xa8,0x51,0xff,0x1d,0x90,0x45,0xff,0x1d,0x90,0x45,0xff,0x22,0xa8,0x51,0xff,0x22,0xa8,0x51,0xff,0x19,0x7b,0x3c};
				static const u8 sigA[32] = {0xd0,0x8b,0xd0,0x8b,0xb0,0x27,0xb0,0x27,0xb0,0x27,0xb0,0x27,0xbc,0x68,0xbc,0x68,0xc4,0x69,0xc4,0x69,0xc8,0x89,0xc8,0x89,0xb4,0x48,0xb4,0x48,0x9c,0x65,0x9c,0x65};
				const u32 preL = 0xff0c6123u, preA = 0xd08bd08bu;
				const u32 ranges[][2] = {{0x100000u,0x10000000u},{0x40000000u,0x41000000u},{0x50000000u,0x51000000u}};
				u32 fL = 0, fA = 0;
				for (const auto& rg : ranges) {
					for (u32 a = rg[0]; a + 32u < rg[1]; a += 4u) {
						const u32 v = reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value();
						if (v == preL) { bool m = true; for (u32 k = 0; k < 32u; k++) if (*(mem_ + u64{a} + k) != sigL[k]) { m = false; break; } if (m && fL < 4u) { printf("[LOGOSCAN] logo pixels @0x%x\n", a); fL++; } }
						else if (v == preA) { bool m = true; for (u32 k = 0; k < 32u; k++) if (*(mem_ + u64{a} + k) != sigA[k]) { m = false; break; } if (m && fA < 4u) { printf("[LOGOSCAN] audio-chunk(inflate) @0x%x\n", a); fA++; } }
					}
				}
				printf("[LOGOSCAN] logo-loaded=%s (%u)  audio-inflate-correct=%s (%u)\n", fL?"YES":"NO", fL, fA?"YES":"NO", fA);
			}}
			// RLTRACE (2026-06-03): RenderLoop(0x1000003) is the consumer of main's channel (wake=idx5).
			// Capture its dispatch channel (r3 @0x40fc78) + count "post 0x96004400" signal hits (0x421524).
			// Tiny signal-count while main waits => RenderLoop stalls BEFORE main's signal command.
			if (this->id == 0x1000003u)
			{
				if (cia == 0x40fc78u) { static thread_local bool s_rc = false; if (!s_rc) { s_rc = true; printf("[RLCHAN] RenderLoop dispatch channel r3=0x%x\n", static_cast<u32>(gpr[3])); } }
				if (cia == 0x42179cu) { static thread_local u32 s_rp = 0; const u32 n = ++s_rp; if (n <= 3u || (n % 128u) == 0u) printf("[RLPROC] RenderLoop enters cmd-processor 0x42179c #%u (chan r3=0x%x)\n", n, static_cast<u32>(gpr[3])); }
				if (cia == 0x421524u) { static thread_local u32 s_sg = 0; const u32 n = ++s_sg; if (n <= 4u || (n % 64u) == 0u) printf("[RLSIGNAL] RenderLoop reached 0x96004400 post #%u (r30=0x%x)\n", n, static_cast<u32>(gpr[30])); }
				// RLOP: 0x410cac per-command dispatch (mtctr@0x410d34) — log opcode r11 + handler r0.
				if (cia == 0x410d34u) { static thread_local u32 s_op = 0; const u32 n = ++s_op; if (n <= 16u || (n % 128u) == 0u) printf("[RLOP] #%u opcode=%u handler=0x%x lr=0x%x\n", n, static_cast<u32>(gpr[11]), static_cast<u32>(gpr[0]), static_cast<u32>(lr)); }
				// RLBCTRL: the REAL bctrl target at 0x4216b0 (mtctr@0x4216a4: r0=target, r9=OPD, r30=object).
				// lr=0x4216b4 idx5 waits happen INSIDE this target — so this is the actual blocking handler.
				if (cia == 0x4216a4u) { static thread_local u32 s_bc = 0; const u32 n = ++s_bc; if (n <= 16u || (n % 256u) == 0u) printf("[RLBCTRL] #%u target=0x%x opd=0x%x obj=0x%x objvt=0x%x\n", n, static_cast<u32>(gpr[0]), static_cast<u32>(gpr[9]), static_cast<u32>(gpr[30]), reinterpret_cast<be_t<u32>*>(mem_ + (u64{static_cast<u32>(gpr[30])} & 0xFFFFFFFFu))->value()); }
				// RLDRAIN: ring header the RenderLoop observes right before bl 0x3c4c58 (check-empty) in the drain
				// loop 0x421708 (r31=channel @0x42172c). Compare read/write indices vs main's committed command.
				if (cia == 0x42172cu) { static thread_local u32 s_dq = 0; const u32 n = ++s_dq; if (n <= 4u || (n % 512u) == 0u) { const u32 ch = static_cast<u32>(gpr[31]); const auto rd = [mem_](u32 a){ return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); }; printf("[RLDRAIN] #%u chan=0x%x ringhdr +18:%08x +1c:%08x +20:%08x +24:%08x +28:%08x +2c:%08x\n", n, ch, rd(ch+0x18), rd(ch+0x1c), rd(ch+0x20), rd(ch+0x24), rd(ch+0x28), rd(ch+0x2c)); } }
				// RLFINE: sample the RenderLoop's live PC to map its actual loop (esp. the last region
				// before it parks). Resolves the RLCHAN/RLDRAIN=0 contradiction — what loop it really runs.
				{ static thread_local u32 s_rf = 0; if ((++s_rf & 0x1FFFFu) == 0u) printf("[RLFINE] cia=0x%x lr=0x%x r3=0x%x\n", static_cast<u32>(cia), static_cast<u32>(lr), static_cast<u32>(gpr[3])); }
			}
			static thread_local u32 s_wtc = 0;
			if ((++s_wtc & 0x7FFFFFu) == 0) // very low rate (~every 8M instr): progress heartbeat, negligible overhead on a long run
			{
				const u32 wr3 = static_cast<u32>(gpr[3]);
				printf("[WORKERCIA] id=0x%x cia=0x%x lr=0x%x r3=0x%x\n", this->id, cia, static_cast<u32>(lr), wr3);
			}
		}
				// RLDUMP: producer-side of the main<->RL deadlock. main (0x1000000) is parked
				// polling 0x27700 for a resource; RL (0x1000006) cycles 0x49bxxx without
				// delivering it. Dump RL's hottest loop fn (0x49beb8) + regs + LR + disasm.
				if (this->id == 0x1000006u)
				{
					static thread_local int s_rld = 0;
					if (cia >= 0x3bfd40u && cia <= 0x3bfdc0u && (s_rld++ & 0x3Fu) == 0)
					{
						s_rld = 1;
						const auto rdr = [mem_](u32 a){ return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
						printf("[RLDUMP] cia=0x%x lr=0x%x r3=0x%x r4=0x%x r5=0x%x r6=0x%x r28=0x%x r29=0x%x r30=0x%x r31=0x%x code:", static_cast<u32>(cia), static_cast<u32>(lr), static_cast<u32>(gpr[3]), static_cast<u32>(gpr[4]), static_cast<u32>(gpr[5]), static_cast<u32>(gpr[6]), static_cast<u32>(gpr[28]), static_cast<u32>(gpr[29]), static_cast<u32>(gpr[30]), static_cast<u32>(gpr[31]));
						for (u32 a = 0x3bfd40u; a <= 0x3bfdc0u; a += 4) printf(" %08x", rdr(a));
						printf(" m@r5:"); { const u32 b5 = static_cast<u32>(gpr[5]) & ~3u; for (u32 a = b5; a <= b5 + 0x20u; a += 4) printf(" %08x", rdr(a)); }
						printf(" m@r31:"); { const u32 b31 = static_cast<u32>(gpr[31]) & ~3u; for (u32 a = b31; a <= b31 + 0x20u; a += 4) printf(" %08x", rdr(a)); }
						printf("\n");
					}
				}
				if (this->id == 0x1000000u)   // DIAG ON: RESSCAN +/-8000 scan + BLKDUMP + MAINCIA (resource-registration wall test)
		{
			g_main_cia_latch = static_cast<u32>(cia); // latch main's live interpreter PC; frozen => where main is parked (read via [MAINLAST] from another thread)
				// MAINSPIN (2026-06-03): post-stream, main's latch freezes at 0x32dd0 (a dt/time fn prologue)
				// while [MAINLAST]="not in syscall" -> main blocks in a HOST wait right after this PC. The
				// `lr` here = the caller loop L (main's real top-level loop); hit-count distinguishes a tight
				// poll-spin (rises fast) from a one-time park (stays ~1). Back-chain walks deeper callers.
				if (cia == 0x32dd0u)
				{
					static thread_local u32 s_ms = 0; const u32 n = ++s_ms;
					if (n <= 4u || (n % 8192u) == 0u)
					{
						const auto rdm = [mem_](u32 a) -> u32 { return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
						const u32 toc = static_cast<u32>(gpr[2]);
						const u32 clkptr = rdm(toc - 0x7888u);           // -0x7888(r2): ptr to current-time double
						const u32 structb = rdm(toc - 0x78acu);          // -0x78ac(r2): base for the +0x2731dc/+0x2731e0 reads
						printf("[MAINSPIN] #%u cia=0x32dd0 lr=0x%x r1=0x%x r2=0x%x clkptr=0x%x clk=%08x%08x structb=0x%x f@31dc=%08x f@31e0=%08x\n",
							n, static_cast<u32>(lr), static_cast<u32>(gpr[1]), toc, clkptr,
							clkptr ? rdm(clkptr) : 0u, clkptr ? rdm(clkptr + 4) : 0u,
							structb, rdm((structb + 0x270000u) + 0x31dcu), rdm((structb + 0x270000u) + 0x31e0u));
						// GENSCALE: read the title-world generator's index scale constants so we can diff vs
						// desktop RPCS3. f3 = f11*C1 is the per-index scale; it comes out 1.0 (no scaling) ->
						// the PRNG index isn't reduced into the buffer range -> overflow -> stack stomp.
						if (n == 1u)
						{
							const u32 toc2 = static_cast<u32>(gpr[2]);
							printf("[GENSCALE] f11(clk@0x%x)=%08x%08x  C1(@0x%x,-0x7628)=%08x%08x  C@-7640=%08x%08x  C@-7620=%08x%08x  [f3=f11*C1; if f3==1.0 the scale is broken]\n",
								clkptr, clkptr ? rdm(clkptr) : 0u, clkptr ? rdm(clkptr + 4) : 0u,
								toc2 - 0x7628u, rdm(toc2 - 0x7628u), rdm(toc2 - 0x7628u + 4u),
								rdm(toc2 - 0x7640u), rdm(toc2 - 0x7640u + 4u),
								rdm(toc2 - 0x7620u), rdm(toc2 - 0x7620u + 4u));
						}
						u32 sp = static_cast<u32>(gpr[1]);
						for (u32 f = 0; f < 8 && sp >= 0x10000u && sp < 0x40000000u; f++)
						{
							const u32 back = rdm(sp + 4); const u32 slr = rdm(sp + 20);
							printf("  [MAINSPIN-FR] #%u sp=0x%x savedLR=0x%x back=0x%x\n", f, sp, slr, back);
							if (back <= sp) break; sp = back;
						}
					}
				}
			// IDX6-DECODE (2026-06-02): one-shot raw disasm of main's frame-sync loop around the
			// sys_semaphore_wait(0x96004600) at lr=0x411150. idx6 is NEVER posted; main livelocks
			// (WAITFORCE limps it). Decode what main re-tests after the wait returns (a memory flag?
			// a counter? the flip count?) + back-chain so we know what subsystem must post idx6.
			if (cia >= 0x411040u && cia <= 0x4111f0u)
			{
				static thread_local bool s_d6 = false;
				if (!s_d6)
				{
					s_d6 = true;
					const auto rdm = [mem_](u32 a) -> u32 { return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
					printf("[IDX6D] r1=0x%x r3=0x%x r4=0x%x r5=0x%x r6=0x%x r7=0x%x r24=0x%x r25=0x%x r26=0x%x r27=0x%x r28=0x%x r29=0x%x r30=0x%x r31=0x%x cia=0x%x lr=0x%x\n",
						static_cast<u32>(gpr[1]),static_cast<u32>(gpr[3]),static_cast<u32>(gpr[4]),static_cast<u32>(gpr[5]),static_cast<u32>(gpr[6]),static_cast<u32>(gpr[7]),static_cast<u32>(gpr[24]),static_cast<u32>(gpr[25]),static_cast<u32>(gpr[26]),static_cast<u32>(gpr[27]),static_cast<u32>(gpr[28]),static_cast<u32>(gpr[29]),static_cast<u32>(gpr[30]),static_cast<u32>(gpr[31]),static_cast<u32>(cia),static_cast<u32>(lr));
					for (u32 p = 0x410fa0u; p < 0x4111f0u; p += 16)
						printf("  [IDX6D] @0x%x: %08x %08x %08x %08x\n", p, rdm(p), rdm(p+4), rdm(p+8), rdm(p+0xc));
					// back-chain stack walk (PPC64 ELFv1: back ptr @[sp+4] low word, saved LR @[sp+20] low word, big-endian 8B slots)
					u32 sp = static_cast<u32>(gpr[1]);
					for (u32 f = 0; f < 14 && sp >= 0x10000u && sp < 0x60000000u; f++) // main stack lives @0x5008xxxx
					{
						const u32 back = rdm(sp + 4);
						const u32 slr = rdm(sp + 20);
						printf("  [IDX6D-FR] #%u sp=0x%x savedLR=0x%x back=0x%x\n", f, sp, slr, back);
						if (back <= sp) break;
						sp = back;
					}
				}
			}
			// CODEDUMP (2026-06-03): one-shot dump of deadlock-relevant code regions from runtime memory
			// (EBOOT segments are encrypted on disc, so disassemble these live bytes offline w/ capstone).
			{
				static thread_local bool s_cd = false;
				if (!s_cd && this->id == 0x1000000u)
				{
					s_cd = true;
					const auto rdm = [mem_](u32 a) -> u32 { return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
					const u32 regions[][2] = {
						{0x426300u, 0x426560u}, // main 0x96004400 wait (final block)
						{0x421400u, 0x421900u}, // RenderLoop cmd dispatcher (jump table + idx5 waits @0x421468/0x4216b4) + drain
						{0x421700u, 0x421860u}, // main handshake @0x4217dc/0x4217bc
						{0x423a80u, 0x423d00u}, // worker wait routine @0x423c00
						{0x63b00u,  0x63e00u},  // usleep poll loop @0x63c98 + caller
						{0x40fb00u, 0x40fe00u}, // RenderLoop idle/consume @0x40fc7c
						{0x3c4a00u, 0x3c4f00u}, // SPSC ring primitives: 0x3c4c58 check-empty, 0x3c4dd4 commit
						{0x410000u, 0x412000u}, // render module: dispatcher 0x410cac + all opcode handlers + idx5-wait handler
						{0x32c00u,  0x33200u},  // SESSION6: main settles at 0x32e7c after the deadlock break — decode this loop
						{0x10f800u, 0x110400u}, // SESSION7: main's top loop — sys_time_get_current_time returns to 0x10feb8 here
						{0x33200u,  0x33800u},  // SESSION7: continuation of the 0x32dd0 dt fn (find its blr + the poll/wait it leads to)
						{0x3f7800u, 0x3f7f80u}, // SESSION7: RenderLoop frame routine — calls cellRescSetConvertAndFlip @lr=0x3f7ae4
						{0x402600u, 0x403200u}, // SESSION7: main's TOP frame loop — calls flip-barrier wait @lr=0x402aac; scene-build is here
						{0x4a5800u, 0x4a5e00u}, // SESSION7: main's RENDER fn — calls flip-sync, returns to 0x4a5ce4; scene-build here
						{0x409600u, 0x409d00u}, // SESSION7: main's GAME LOOP — calls render @0x4097e0/0x409b78
						{0x27580u,  0x27780u},  // SESSION7: main's resource-PRESENCE poll (entry 0x2761c, loop 0x27638, found-handler 0x2766c) — decode the EXACT table lookup/compare offset
						{0x69040u,  0x69180u},  // SESSION7: the register fn (cnt write @0x690b4) — how registration works
						{0xe0140u,  0xe0280u},  // SESSION7: the unrolled registration driver START (cnt 1.. @lr=0xe0164)
						{0xe7880u,  0xe7d80u},  // SESSION7: registration driver END — last commit @lr=0xe795c (cnt=342). Does it continue (stalled) or return (complete)?
						{0x1d6200u, 0x1d6500u},  // SESSION7: resource-manager — calls static manifest @0x1d6270, returns 0x1d6274. POST-manifest = load/re-commit logic?
						{0x1d6e00u, 0x1d7080u},  // SESSION7: resource-manager outer (0x1d6fec calls 0x1d6274)
						{0x1d4700u, 0x1d4c80u},  // SESSION7: DYNAMIC register-setup path (0x86a40 callers 0x1d4798/0x1d49e0/0x1d4a84/0x1d4b1c) — does it commit?
						{0x275c0u,  0x275f8u},  // SESSION7: poll pre-check 0x275f8 (data-loaded? returns 0=loaded)
						{0x425680u, 0x425a00u},  // SESSION7: loader completion-dispatch ~0x4257b4 (LDRFINE lr=0x42570c) — the post-load callback that should register?
						{0x4929c0u, 0x492c40u},  // SESSION9: the {u16,u16,0,0} table-writer loop (cia 0x492aa8 ~68x) — suspected stack-overrun corruptor of main's saved-LR slot
						{0x8b800u, 0x92900u},  // SESSION9: the sth-loop (cia 0x90xxx-0x92xxx) writing 0x18/0x1c into stack scratch @frame+0xe0 that overruns the saved-LR slot @frame+0x120 (0x500806b0)
						{0x8b400u, 0x8b800u},  // SESSION9: id=0xe7 jump-table HANDLER @0x8b5cc — does it restore r1 (+0x70)?
						{0x3ff780u, 0x3ffa00u},  // SESSION9 post-breakthrough: NEW null-fn call site 0x3ff8a4 (r2=0xf00 garbage TOC -> unconstructed object); what object/dispatch?
						{0x86a00u, 0x87400u},  // SESSION9: 0x86a40 = bridge REGFN scratch -> INIT (computes the +0xe0/+0x80 struct dest)
						{0x1d4400u, 0x1d4710u},  // SESSION9: REGFN @0x1d48xx PROLOGUE (frame size + does it save LR to +0x120?)
					};
					for (const auto& rg : regions)
						for (u32 b = rg[0]; b < rg[1]; b += 32)
							printf("[CODEDUMP] %06x: %08x %08x %08x %08x %08x %08x %08x %08x\n", b,
								rdm(b), rdm(b+4), rdm(b+8), rdm(b+0xc), rdm(b+0x10), rdm(b+0x14), rdm(b+0x18), rdm(b+0x1c));
				}
			}
			// CHANDUMP (2026-06-03): dump main's actor/channel object on entry to the 0x96004400 wait
			// (0x426434: r28=channel). Decode the producer/consumer link: vtable(+0), queue rd-ptr(+0xa8)/
			// count(+0xac), completion sem(+0xe4), consumer-wake sem ptr(+0xe8), flags(+0xc8/+0xcc/+0xd4).
			if (cia == 0x426460u && this->id == 0x1000000u)
			{
				static thread_local bool s_ch = false;
				if (!s_ch)
				{
					s_ch = true;
					const auto rdm = [mem_](u32 a) -> u32 { return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
					const u32 ch = static_cast<u32>(gpr[28]);
					printf("[CHANDUMP] main channel r28=0x%x r27=0x%x r29=0x%x\n", ch, static_cast<u32>(gpr[27]), static_cast<u32>(gpr[29]));
					for (u32 o = 0; o <= 0x100u; o += 16)
						printf("  [CHAN] @+0x%x: %08x %08x %08x %08x\n", o, rdm(ch+o), rdm(ch+o+4), rdm(ch+o+8), rdm(ch+o+0xc));
					const u32 vt = rdm(ch);
					printf("  [CHAN-VT] vtable=0x%x: %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", vt,
						rdm(vt), rdm(vt+4), rdm(vt+8), rdm(vt+0xc), rdm(vt+0x10), rdm(vt+0x14), rdm(vt+0x18), rdm(vt+0x1c), rdm(vt+0x20));
					const u32 wp = rdm(ch+0xe8);
					printf("  [CHAN-WAKE] obj=0x%x: type=%08x sem_id=%08x (this is main's consumer-wake sem)\n", wp, rdm(wp), rdm(wp+4));
					const u32 q = rdm(ch+0x18);
					for (u32 o = 0; o <= 0x60u; o += 16)
						printf("  [CHAN-Q] queue=0x%x +0x%x: %08x %08x %08x %08x\n", q, o, rdm(q+o), rdm(q+o+4), rdm(q+o+8), rdm(q+o+0xc));
					// pending command is a C++ object: cmd+0x14 -> obj -> vtable -> vtable[0]={fn,toc}
					const u32 cmdobj = rdm(q+0x14);   // 0x592a58
					const u32 cvt    = rdm(cmdobj);   // 0x59bb70 (vtable)
					const u32 hfn    = rdm(cvt) & ~3u;// vtable[0] fn = real handler (0x4116b8)
					printf("[CMD-HANDLER] cmd=0x%x type=%d obj=0x%x vtable=0x%x fn0=0x%x m1=0x%x m2=0x%x m3=0x%x\n",
						q, static_cast<int>(rdm(q+0x20)), cmdobj, cvt, rdm(cvt), rdm(cvt+8), rdm(cvt+0x10), rdm(cvt+0x18));
					if (hfn >= 0x10000u && hfn < 0x588ce8u)
						for (u32 b = hfn; b < hfn + 0x240u; b += 32)
							printf("[CODEDUMP] %06x: %08x %08x %08x %08x %08x %08x %08x %08x\n", b,
								rdm(b), rdm(b+4), rdm(b+8), rdm(b+0xc), rdm(b+0x10), rdm(b+0x14), rdm(b+0x18), rdm(b+0x1c));
				}
			}
			// GAMECLOCK (2026-06-03): the game's time double lives at *0x4e9980 (main read f11 from it @0x32dd0).
			// Counter-based (cia probes are unreliable) — dump it over main's execution to test if the clock
			// is advancing. If stuck near zero, main's time-based state machine never reaches the splash.
			{
				static thread_local u32 s_g6 = 0;
				if ((++s_g6 & 0x1FFFFu) == 0u)
				{
					const auto rdm = [mem_](u32 a) -> u32 { return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
					printf("[GAMECLOCK] n=%u time@0x4e9980=%08x%08x cia=0x%x\n", s_g6, rdm(0x4e9980u), rdm(0x4e9984u), static_cast<u32>(cia));
					static thread_local u32 s_inrange = 0; static thread_local bool s_dumped = false;
					const u32 c = static_cast<u32>(cia);
					if (c >= 0x32c00u && c < 0x33400u) s_inrange++; else s_inrange = 0;
					if (!s_dumped && s_inrange >= 8u)
					{
						s_dumped = true;
						printf("[STALL] cia=0x%x lr=0x%x r0=0x%x r3=0x%x r4=0x%x r5=0x%x r9=0x%x r10=0x%x r28=0x%x r29=0x%x r30=0x%x r31=0x%x\n",
							c, static_cast<u32>(lr), static_cast<u32>(gpr[0]), static_cast<u32>(gpr[3]), static_cast<u32>(gpr[4]), static_cast<u32>(gpr[5]), static_cast<u32>(gpr[9]), static_cast<u32>(gpr[10]), static_cast<u32>(gpr[28]), static_cast<u32>(gpr[29]), static_cast<u32>(gpr[30]), static_cast<u32>(gpr[31]));
						u32 sp = static_cast<u32>(gpr[1]);
						for (u32 f = 0; f < 14 && sp >= 0x10000u && sp < 0x60000000u; f++)
						{
							const u32 back = rdm(sp + 4); const u32 slr = rdm(sp + 20);
							printf("  [STALL-FR] #%u sp=0x%x savedLR=0x%x\n", f, sp, slr);
							if (back <= sp) break; sp = back;
						}
					}
				}
			}
			// WAITLOOP-DECODE (2026-06-03): main is PAST idx6 but idles in a sys_timer_usleep(20ms) loop
			// @lr=0x63c98, presenting EMPTY frames (scene draws have verts=0). Decode what flag it re-tests.
			if (cia == 0x63c98u)
			{
				static thread_local bool s_dw = false;
				if (!s_dw)
				{
					s_dw = true;
					const auto rdm = [mem_](u32 a) -> u32 { return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
					printf("[WAITLOOP] r3=0x%x r4=0x%x r5=0x%x r6=0x%x r24=0x%x r25=0x%x r26=0x%x r27=0x%x r28=0x%x r29=0x%x r30=0x%x r31=0x%x cia=0x%x lr=0x%x\n",
						static_cast<u32>(gpr[3]),static_cast<u32>(gpr[4]),static_cast<u32>(gpr[5]),static_cast<u32>(gpr[6]),static_cast<u32>(gpr[24]),static_cast<u32>(gpr[25]),static_cast<u32>(gpr[26]),static_cast<u32>(gpr[27]),static_cast<u32>(gpr[28]),static_cast<u32>(gpr[29]),static_cast<u32>(gpr[30]),static_cast<u32>(gpr[31]),static_cast<u32>(cia),static_cast<u32>(lr));
					printf("[WAITFLAG] @0x63c98 lr=0x%x  arg-hunt reg : *(reg+0x18):\n", static_cast<u32>(lr));
					for (u32 ri = 3; ri <= 31; ri++) {
						const u32 rv = static_cast<u32>(gpr[ri]);
						if (rv >= 0x10000u && rv < 0x10000000u)
							printf("  [WAITFLAG] r%u=0x%x  +0x18=0x%x  +0x0=0x%x +0x4=0x%x +0x14=0x%x +0x1c=0x%x +0x10=0x%x\n",
								ri, rv, rdm(rv+0x18), rdm(rv), rdm(rv+4), rdm(rv+0x14), rdm(rv+0x1c), rdm(rv+0x10));
					}
				}
				// WAITFORCE (2026-06-03): past the 152-frame intro, set the flag main spins on to 1 ONCE,
				// a diagnostic to see what is behind the gate (does main submit the splash, and via which path?).
				{
					static thread_local u32 s_hits = 0;
					static thread_local bool s_forced = false;
					if (!s_forced && ++s_hits == 15u)
					{
						s_forced = true;
						const auto rdm = [mem_](u32 a) -> u32 { return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
						const u32 a31 = static_cast<u32>(gpr[31]);
						const u32 a30 = static_cast<u32>(gpr[30]);
						printf("[WAITFORCE] hit=600 r31=0x%x *(r31+0x18)=0x%x  r30=0x%x *(r30+0x18)=0x%x\n",
							a31, rdm(a31+0x18), a30, rdm(a30+0x18));
						for (u32 p = 0x63c40u; p <= 0x63ca0u; p += 16)
							printf("  [WF-DIS] @0x%x: %08x %08x %08x %08x\n", p, rdm(p), rdm(p+4), rdm(p+8), rdm(p+0xc));
						// NOTE: forcing *(r31+0x18)=1 here (tried) makes main exit the poll then dispatch a
						// NULL fn (exec_task cia=0x0) -> OOB trap. The flag gates a real async completion
						// whose result table is empty (dead SPU pipeline) — do NOT force it. Diagnostic only.
					}
				}
			}
			// MAINFREEZE: main's interpreter parks at 0x9c6f74. Dump the instruction + regs + lr + ctr
			// to decode whether it's a branch-to-self spin, an HLE call (bl), or a load-test loop.
			// MAINFREEZE (dump-only, NO unblock): main bctrl's through an UNRESOLVED import
			// descriptor (entry=desc+4, toc=0) at 0x9c6f74. blr-emulating it just cascades
			// into more null-fn calls (the whole import table of the 0x9c0000 module is
			// unpopulated) — so leave main frozen here and fix the missing library instead.
			// Log EVERY distinct HLE code-addr main calls in the wedge region (0x9c6800-0x9c7000),
			// with its function name+ptr. main enters the wedged HLE fn once and blocks inside it
			// (no re-loop), so fire on first hit per distinct addr. The wedge = the last one before silence.
			if (cia >= 0x9c6f40u && cia <= 0x9c6fffu && (cia & 7u) == 4u)
			{
				static thread_local u32 s_seen[8] = {0,0,0,0,0,0,0,0}; static thread_local u32 s_n = 0;
				bool dup = false; for (u32 i = 0; i < s_n && i < 8; i++) if (s_seen[i] == static_cast<u32>(cia)) dup = true;
				if (!dup && s_n < 8)
				{
					s_seen[s_n++] = static_cast<u32>(cia);
					extern std::vector<std::string> g_ppu_function_names;
					const u32 hb = g_fxo->get<ppu_function_manager>().addr;
					const u32 idx = (static_cast<u32>(cia) - hb) / 8;
					const auto& fns = ppu_function_manager::get();
					const void* fp = (idx < fns.size()) ? reinterpret_cast<const void*>(fns[idx]) : nullptr;
					const char* nm = (idx < g_ppu_function_names.size() && !g_ppu_function_names[idx].empty()) ? g_ppu_function_names[idx].c_str() : "<noname>";
					printf("[MAINFREEZE] HLE-call cia=0x%x lr=0x%x idx=%u name='%s' fnptr=%p fncount=%u\n",
						static_cast<u32>(cia), static_cast<u32>(lr), idx, nm, fp, static_cast<unsigned>(fns.size()));
					if (lr >= 0x10000 && lr < 0x588ce8)
					{
						const auto rdm = [mem_](u32 a) -> u32 { return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
						u32 start_p = (static_cast<u32>(lr) - 0x50u) & ~3u;
						printf("[MAINFREEZE-LR] code around lr=0x%x start=0x%x:\n", static_cast<u32>(lr), start_p);
						for (u32 i = 0; i < 11; i++)
						{
							u32 p = start_p + i * 16;
							printf("  [LR-DIS] @0x%x: %08x %08x %08x %08x\n", p, rdm(p), rdm(p+4), rdm(p+8), rdm(p+0xc));
						}
					}
				}
			}
			// HANDSHAKE-LOOP dump: main loops sem_post(0x96006700)@0x4217dc <-> sem_wait@0x426460
			// + cellKbGetInfo, never exiting to the flip. Dump the loop body + regs once to find the
			// exit branch / loop counter so main can be forced to the present phase next.
			if (cia == 0x4217dcu)
			{
				static thread_local bool s_hd = false;
				if (!s_hd)
				{
					s_hd = true;
					const auto rd = [mem_](u32 a){ return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
					// The post-fn (0x4217bc) saved its caller's return LR at [r1+0x90]; that's main's
					// top loop. Dump code around it to find the loop's exit branch / poll condition.
					const u32 caller = rd(static_cast<u32>(gpr[1]) + 0x90u);
					printf("[HANDSHAKE] @0x4217dc caller=0x%x r1=0x%x r29=0x%x r30=0x%x r31=0x%x | caller code@0x%x:",
						caller, static_cast<u32>(gpr[1]), static_cast<u32>(gpr[29]), static_cast<u32>(gpr[30]), static_cast<u32>(gpr[31]), caller - 0x60u);
					for (u32 a = (caller - 0x60u) & ~3u; a <= caller + 0x30u; a += 4) printf(" %08x", rd(a));
					printf("\n");
				}
			}
			// One-shot: dump main's block region so we can decode what it waits on
			// (SPURS wait? thread join? poll loop?). cia clusters ~0x27788 (lr 0x27638)
			// just before main goes silent.
			// RESSCAN: definitive test of the resource-registration wall. When main
			// busy-polls at 0x27700, scan the handle table (+/-200 entries of 0x68c
			// stride around main's cursor r29) and count entries whose +0x60 halfword
			// is REGISTERED (!= 0xffff), throttled. If `registered` stays 0 for the
			// whole boot, NO PPU thread ever registers a resource (-> SPU/SPURS-gated,
			// the fundamental wall). If it rises, main waits for a specific id (target
			// = halfword @[r31+0x6c]) we can identify and hand-register as a fallback.
			if (cia == 0x27700u)   /* POLLBYPASS5 ON: real poll never produces 0x8xxxxx resources; fake-found needed to reach cellResc */
			{
				// (HANDREG reverted: forcing [entry+0x60] didn't advance main — the table is a
				// LIVE/dynamic structure main+loader churn, and main waits for the loader to
				// genuinely complete each resource, not just for the flag. Over-wide writes also
				// corrupted adjacent memory. The real gate is the loader's per-resource decode.)
				{ static thread_local u32 s_rcnt = 0; if ((s_rcnt++ & 0x3Fu) == 0u) { const u32 curx = static_cast<u32>(gpr[29]); const auto rdhx = [mem_](u32 a){ return reinterpret_cast<be_t<u16>*>(mem_ + u64{a})->value(); }; u32 regx = 0; for (s32 k = -300; k <= 300; k++) { const s64 e = static_cast<s64>(curx) + static_cast<s64>(k) * 0x68c; if (e < 0x10000 || e + 0x62 >= 0x5ff00000) continue; if (rdhx(static_cast<u32>(e) + 0x60) != 0xffff) regx++; } printf("[REGCOUNT] cur=0x%x reg=%u/601\n", static_cast<unsigned>(curx), static_cast<unsigned>(regx)); } }
				// POLLBYPASS5: redirect stuck-id searches to the found-handler 0x2766c. This is
				// the INNER per-instruction loop — unlike the outer-loop cia=lr bypasses, it
				// actually sees 0x27700 (RESSCAN here proves it). r31=descriptor; the target id
				// = [r31+0x6c] (the exact read RESSCAN uses, which catches all 7 stuck ids).
				// 0x2766c (li r6,1; stb r6,0x15(r31); li r3,1; b epilogue) returns "found"; the
				// prologue already ran so the epilogue tears the frame down cleanly.
				{
					const u32 r31q = static_cast<u32>(gpr[31]);
					if (r31q >= 0x10000u && r31q < 0x5ff00000u)
					{
						const u16 tq = reinterpret_cast<be_t<u16>*>(mem_ + (u64{r31q} + 0x6cu))->value();
						{ // IDSCAN (2026-06-03): authoritative — search the WHOLE table for main's tq; count
						  // entries with a REAL id (+0x60 != 0xffff AND != 0). found -> H-bug (offset); absent -> empty table.
							const u32 curs = static_cast<u32>(gpr[29]);
							static thread_local u32 s_ids = 0;
							if (curs >= 0x800000u && curs < 0xb00000u && tq != 0u && (s_ids++ < 4u || (s_ids & 0x3FFu) == 0u))
							{
								const auto rdh = [mem_](u32 a){ return reinterpret_cast<be_t<u16>*>(mem_ + u64{a})->value(); };
								u32 realreg = 0; s32 fk = 0x7fffffff; u32 foff = 0; u16 f60f = 0;
								for (s32 k = -4000; k <= 4000; k++)
								{
									const s64 e = static_cast<s64>(curs) + static_cast<s64>(k) * 0x68c;
									if (e < 0x10000 || e + 0x80 >= 0x5ff00000) continue;
									const u32 ea = static_cast<u32>(e);
									const u16 f60 = rdh(ea + 0x60);
									if (f60 != 0xffffu && f60 != 0u) realreg++;
									for (u32 off = 0x40u; off <= 0x78u; off += 2u) if (rdh(ea + off) == tq) { if (fk == 0x7fffffff) { fk = k; foff = off; f60f = f60; } }
								}
								printf("[IDSCAN] tq=0x%x cur=0x%x realreg=%u foundk=%d foundoff=0x%x f60@=0x%04x\n", static_cast<unsigned>(tq), curs, realreg, static_cast<int>(fk), foff, f60f);
								// IDSCAN2: the CORRECT two-level structure decoded from the poll asm @0x27700.
								// inner array @ r10g + 0x290000 - 0x6f8c, stride 0x80, id halfword @ offset 0.
								{
									const u32 r2v = static_cast<u32>(gpr[2]);
									const u32 r10g = reinterpret_cast<be_t<u32>*>(mem_ + (u64{r2v} - 0x78acu))->value();
									const u32 inner = r10g + 0x290000u - 0x6f8cu;
									const u32 cnt = reinterpret_cast<be_t<u32>*>(mem_ + (u64{r10g} + 0x290000u - 0x6f9cu))->value(); // registered count @0x880694
									s32 fk2 = -1; u32 nz = 0;
									char ids[600]; u32 p = 0;
									for (u32 i = 0; i < 256u && p < 560u; i++) { const u32 a = inner + i * 0x80u; const u16 v = rdh(a); if (v != 0u && v != 0xffffu) { nz++; p += static_cast<u32>(snprintf(ids + p, sizeof(ids) - p, "%x:%04x ", i, v)); } if (v == tq && fk2 < 0) fk2 = static_cast<s32>(i); }
									ids[p] = 0;
									printf("[IDSCAN2] tq=0x%x cnt=%u inner=0x%x found_i=%d nz256=%u  idx:id = %s\n",
										static_cast<unsigned>(tq), cnt, inner, static_cast<int>(fk2), nz, ids);
								}
							}
						}
						{ // IDDUMP (2026-06-03): one-shot — dump +0x50..0x70 of REGISTERED entries; do main's
						  // wanted IDs (0x8/0xe1/0x6/0x96/0x131) exist at SOME offset (H-bug) or are ABSENT (H-logic)?
							static thread_local bool s_idd = false;
							const u32 curd = static_cast<u32>(gpr[29]);
							if (!s_idd && curd >= 0x800000u && curd < 0xb00000u) // real table cursor (REGCOUNT range), not the 0x1f garbage polls
							{
								s_idd = true;
								const auto rdh = [mem_](u32 a){ return reinterpret_cast<be_t<u16>*>(mem_ + u64{a})->value(); };
								printf("[IDDUMP] main wants tgt=0x%x cur=0x%x. Registered entries (+50..+70):\n", static_cast<unsigned>(tq), curd);
								u32 nn = 0;
								for (s32 k = -400; k <= 400 && nn < 16; k++)
								{
									const s64 e = static_cast<s64>(curd) + static_cast<s64>(k) * 0x68c;
									if (e < 0x10000 || e + 0x80 >= 0x5ff00000) continue;
									const u32 ea = static_cast<u32>(e);
									if (rdh(ea + 0x60) != 0xffff)
									{
										printf("  [IDDUMP] k=%d:", k);
										for (u32 off = 0x50u; off <= 0x70u; off += 2u) printf(" %04x", rdh(ea + off));
										printf("\n");
										nn++;
									}
								}
							}
						}
						// BROAD: redirect EVERY main resource-presence check to "found". The
						// never-registered id set is dynamic (varies per scan), so hardcoding is
						// wrong. Registered ids reach 0x2766c anyway (same result); only the
						// unregistered ones are actually changed. Tests whether clearing the whole
						// resource-presence wait lets main reach the flip. (If it crashes early,
						// gate on a poll-count threshold so early legit checks run normally.)
						{
							// (POLLBYPASS5 redirect restored after the run90 experiment, which PROVED main never
							// posts 0x96006700 regardless of the bypass — main just busy-polls IDs 0x8/0xe1/0x6/
							// 0x96/0x131 from its loop @0x27638; the bypass papers it. Producer of phase-2 is elsewhere.)
							static thread_local u32 s_byp5 = 0;
							if ((s_byp5++ & 0xFFFFFFu) == 0u) printf("[POLLBYPASS5] broad-redirect tgt=0x%x 0x27700->0x2766c n=%u\n", static_cast<unsigned>(tq), s_byp5);
							cia = 0x2766cu;
							continue;
						}
					}
				}
				static thread_local u32 s_rsc = 0;
				if ((++s_rsc & 0x3FFF) == 0)
				{
					const u32 cur = static_cast<u32>(gpr[29]);
					const auto rdh = [mem_](u32 a){ return reinterpret_cast<be_t<u16>*>(mem_ + u64{a})->value(); };
					u32 reg = 0; s32 firstk = 0; u32 firstv = 0;
					for (s32 k = -200; k <= 200; k++)
					{
						const s64 e = static_cast<s64>(cur) + static_cast<s64>(k) * 0x68c;
						if (e < 0x10000 || e + 0x62 >= 0x5ff00000) continue;
						const u16 h = rdh(static_cast<u32>(e) + 0x60);
						if (h != 0xffff) { if (!reg) { firstk = k; firstv = h; } reg++; }
					}
					const u16 tgt2 = rdh(static_cast<u32>(gpr[31]) + 0x6c);
						s32 foundk = 0x40000000; u32 foundcnt = 0; u32 foundoff = 0;
						for (s32 kk = -8000; kk <= 8000; kk++) { const s64 ee = static_cast<s64>(cur) + static_cast<s64>(kk) * 0x68c; if (ee < 0x10000 || ee + 0x80 >= 0x5ff00000) continue; for (u32 off = 0x50u; off <= 0x70u; off += 2u) { if (rdh(static_cast<u32>(ee) + off) == tgt2) { foundcnt++; if (foundk == 0x40000000) { foundk = kk; foundoff = off; } break; } } }
						const u32 r31a = static_cast<u32>(gpr[31]);
						printf("[RESSCAN] cur=0x%x reg=%u/401 firstk=%d firstv=0x%x target=0x%x foundk=%d foundoff=0x%x foundcnt=%u desc6x=%04x %04x %04x %04x %04x %04x %04x\n", cur, reg, static_cast<int>(firstk), firstv, tgt2, static_cast<int>(foundk), foundoff, foundcnt, rdh(r31a+0x64), rdh(r31a+0x66), rdh(r31a+0x68), rdh(r31a+0x6a), rdh(r31a+0x6c), rdh(r31a+0x6e), rdh(r31a+0x70));
				}
			}
			// BLKDUMP2: after the broad 0x27700 redirect, main advanced past the resource
				// wall (reached 0x142148 / 0x409a90 r3=1) but is now stuck at a NEW wall 0x2725c
				// (a different function, called from 0x68e40). Re-fire a dump of 0x27200-0x272fc.
				// POLLBYPASS6: main's deep-init resource-load poll at 0x418e0c/0x418e24 loops
				// (usleep 10ms) while the status word [r31+0x18] == 1 (loading); the loader,
				// blocked on gcm/rwlock, never sets it to 2 (done). Force [r31+0x18]=2 so main
				// sees the load complete and proceeds toward cellGcmInit/flip. Same pattern as the
				// 0x27700 redirect, but for the data-load status word instead of the presence check.
				if (cia == 0x418e0cu || cia == 0x418e24u)
				{
					const u32 r31w = static_cast<u32>(gpr[31]);
					if (r31w >= 0x10000u && r31w < 0x5ff00000u &&
						reinterpret_cast<be_t<u32>*>(mem_ + (u64{r31w} + 0x18u))->value() == 1u)
					{
						*reinterpret_cast<u8*>(mem_ + (u64{r31w} + 0x1bu)) = 2u; // BE u32 [r31+0x18]: 1 -> 2 (done)
						static thread_local u32 s_byp6 = 0;
						if ((s_byp6++ & 0xFFFu) == 0u) printf("[POLLBYPASS6] force load-done r31=0x%x n=%u\n", static_cast<unsigned>(r31w), s_byp6);
					}
				}
				// STUCKSKIP: main consistently stalls re-processing a single bad/bogus object
				// (r3 ~0x880-0x948xxx, varies per run) in the resource-registration method
				// (entry 0x86a40, called from the 0xe0c30 register loop) — the last phase before
				// the GCM render. Detect the SAME object (r3) re-entering 0x86a40 too many times
				// (stuck) and skip it: return success (r3=0) to the caller so the register loop
				// advances to the next object and completes registration toward the flip. main is
				// at the method ENTRY here (lr=caller), so this skips the whole method cleanly.
				if (false && (cia == 0x86a40u || cia == 0x86b90u || cia == 0x492aa8u || cia == 0x492b64u || cia == 0x492be8u || cia == 0x492f34u) && this->id == 0x1000000u)  // STUCKSKIP RETIRED (2026-06-04): this WAS the null-loop root cause — `cia=lr` at 0x86b90 hijacked the (correct) jump-table dispatch on legit repeated registration calls. Dispatch runs natively now.
				{
					static thread_local u32 s_sk_r3 = 0xffffffffu;
					static thread_local u32 s_sk_n = 0;
					const u32 r3v = static_cast<u32>(gpr[3]);
					if (r3v == s_sk_r3)
					{
						if (++s_sk_n > 800u)
						{
							s_sk_n = 0;
							gpr[3] = 0;
							static thread_local u32 s_skl = 0;
							if ((s_skl++ & 0x3u) == 0u) printf("[STUCKSKIP] reg skip obj r3=0x%x -> lr=0x%x n=%u\n", static_cast<unsigned>(r3v), static_cast<unsigned>(lr), s_skl);
							cia = static_cast<u32>(lr) & ~3u;
							continue;
						}
					}
					else { s_sk_r3 = r3v; s_sk_n = 0; }
				}
				if (cia == static_cast<u32>(lr) && cia >= 0x68d00u && cia < 0x69000u) { static thread_local u32 s_csl_c = 0xffffffffu; static thread_local u32 s_csl_n = 0; if (cia == s_csl_c) { if (++s_csl_n > 50000u) { s_csl_n = 0; static thread_local u32 s_csll = 0; if ((s_csll++ & 0x7u) == 0u) printf("[CSLBRK] step self-loop cia=0x%x r3=0x%x\n", static_cast<unsigned>(cia), static_cast<unsigned>(gpr[3])); cia = cia + 4u; continue; } } else { s_csl_c = cia; s_csl_n = 0; } }
				static thread_local u32 s_glc = 0xffffffffu; static thread_local u32 s_gln = 0; const u32 s_blk = cia & ~0xFFFu; const bool stuckhit = (s_blk == s_glc) ? (++s_gln > 3000000u) : (s_glc = s_blk, s_gln = 0u, false);
				if (stuckhit)
				{
					const auto rdb2 = [mem_](u32 a){ return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
					printf("[BLKDUMP2] main spin cia=0x%x lr=0x%x r3=0x%x r4=0x%x r5=0x%x r9=0x%x r30=0x%x r31=0x%x CODE:", cia, static_cast<u32>(lr), static_cast<u32>(gpr[3]), static_cast<u32>(gpr[4]), static_cast<u32>(gpr[5]), static_cast<u32>(gpr[9]), static_cast<u32>(gpr[30]), static_cast<u32>(gpr[31]));
					for (u32 a = (cia & ~3u) - 16u; a <= (cia & ~3u) + 0x18u; a += 4) printf(" %08x", rdb2(a));
					printf(" OBJ:");
					{ const u32 ob = static_cast<u32>(gpr[31]) & ~3u; for (u32 a = ob; a <= ob + 0x40u; a += 4) printf(" %08x", rdb2(a)); } s_gln = 0u; if ((cia >= 0x490000u && cia < 0x4a0000u) || (cia >= 0x680000u && cia < 0x6a0000u) || (cia >= 0x860000u && cia < 0x870000u)) { printf("\n[GENSKIP] skip cia=0x%x r3=0x%x -> lr=0x%x\n", static_cast<unsigned>(cia), static_cast<unsigned>(gpr[3]), static_cast<unsigned>(lr)); gpr[3] = 0; cia = static_cast<u32>(lr) & ~3u; continue; }
					printf("\n");
				}
				static thread_local u32 s_blkd = 0;
			if (cia >= 0x27600u && cia < 0x277c0u && (s_blkd++ & 0x1FFFFu) == 0u)
			{
				s_blkd = 1;
				const auto rdb = [mem_](u32 a){ return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
				printf("[BLKDUMP] main cia=0x%x lr=0x%x r3=0x%x:", cia, static_cast<u32>(lr), static_cast<u32>(gpr[3]));
				for (u32 a = 0x27600; a <= 0x277c0; a += 4) printf(" %08x", rdb(a));
				printf("\n");
			}
			// POLL-BYPASS: main's "is resource registered" fn (entry 0x2761c) is a pure
				// PRESENCE check — it loads the target id from [r31+0x6c], scans the handle
				// table (stride 0x68c) comparing id halfwords, and returns r3=1 (also stores 1
				// to [r31+0x15]) when found, else 0. NO handle is dereferenced. ~6-7 ids
				// (0x1ef/0x131/0x223/0x228/0x233/0x23f/0x26b) never register (their stream
				// never completes), so main polls 0x27700 forever and never flips. Every return
				// compare sites are 0x27730 (cmpw r6,r7) / 0x276bc (cmpw r11,r7); force reg==r7 for ONLY
				// those stuck ids (and only when it would otherwise be not-found) so main can
				// advance past the resource wall toward the flip. No table-memory surgery.
				if (cia == 0x27730u || cia == 0x276bcu)
				{
					const u32 r31b = static_cast<u32>(gpr[31]);
					const u16 tgtb = static_cast<u16>(gpr[7]);
					if (tgtb == 0x1efu || tgtb == 0x131u || tgtb == 0x223u || tgtb == 0x228u || tgtb == 0x233u || tgtb == 0x23fu || tgtb == 0x26bu)
					{
						if (cia == 0x27730u) gpr[6] = gpr[7]; else gpr[11] = gpr[7];
						static thread_local u32 s_byp = 0;
						if ((s_byp++ & 0x3Fu) == 0u) printf("[POLLBYPASS] forced match tgt=0x%x r31=0x%x cia=0x%x n=%u\n", static_cast<unsigned>(tgtb), static_cast<unsigned>(r31b), static_cast<unsigned>(cia), s_byp);
					}
				}
				static thread_local u32 s_mtc = 0;
			if ((++s_mtc & 0xFFFFF) == 0)
			{
				const u32 r3v = static_cast<u32>(gpr[3]);
				const u32 v = r3v < 0x60000000 ? reinterpret_cast<be_t<u32>*>(mem_ + u64{r3v})->value() : 0xbadbad;
				// poll target: the search at 0x27700 compares an array halfword (+0x60)
				// against extsh([r31+0x6c]). Dump it + [r31+0] so we can tell whether the
				// value main waits for is producible (PPU thread) or SPURS-task-only.
				const u32 r31v = static_cast<u32>(gpr[31]);
				const auto rdm = [mem_](u32 a){ return a < 0x5fff0000 ? reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value() : 0xbadbadu; };
				const u32 r29v = static_cast<u32>(gpr[29]);
				// r27 is the init-loop END bound (cmpw r27,r29 @0x1a5748/0x1a5798); r25/r28
				// are the loop's base/cursor. Dump them to gauge whether main's big object-
				// init loop (0x1a5730 calling 0x33f24) is FINITE-and-near-done or effectively
				// unbounded — decides "give it more wall-clock" vs "HLE the init loop".
				const u32 r25v = static_cast<u32>(gpr[25]);
				const u32 r27v = static_cast<u32>(gpr[27]);
				const u32 r28v = static_cast<u32>(gpr[28]);
				printf("[MAINCIA] cia=0x%x lr=0x%x r3=0x%x [r3]=0x%x r29=0x%x [r29+60]=0x%x r31=0x%x [r31+6c]=0x%x r25=0x%x r27=0x%x r28=0x%x\n",
					cia, static_cast<u32>(lr), r3v, v,
					r29v, rdm(r29v + 0x60), r31v, rdm(r31v + 0x6c), r25v, r27v, r28v);
				// LDUMP: cross-thread peek at the loader threads' sticky last_function
				// (the HLE function they're blocked in — survives the block). The
				// WorkerThread runs almost no PPU code so this is the only way to see
				// what HLE call its boot stalls in (data.vfs is never read).
				for (u32 ldid = 0x1000006u; ldid <= 0x100000cu; ldid++)
				{
					if (auto th = idm::get_unlocked<named_thread<ppu_thread>>(ldid))
					{
						printf("[LDUMP] id=0x%x cia=0x%x last_fn=%s\n", ldid,
							static_cast<u32>(th->cia), th->last_function ? th->last_function : "(null)");
					}
				}
				// One-shot: dump the resource-object header (r29..+0x60) so we can
				// see if it's a loaded texture descriptor (real data ptr/size) or
				// an empty/zeroed slot — decides honest-mark vs needs-loading.
				static thread_local int s_objd = 0;
				const u32 ob = static_cast<u32>(gpr[29]);
				if (s_objd++ == 0 && ob >= 0x10000 && ob < 0x60000000)
				{
					const auto rd = [mem_](u32 a){ return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
					printf("[OBJDUMP] r29=0x%x:", ob);
					for (u32 o = 0; o <= 0x60; o += 4) printf(" +%02x=%08x", o, rd(ob + o));
					printf("\n");
					const auto rdi = [mem_](u32 a){ return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
					printf("[ASMDUMP] poll@0x33f00:");
					for (u32 a = 0x33f00; a <= 0x33f90; a += 4) printf(" %08x", rdi(a));
					printf("\n[ASMDUMP] iter@0x1a5730:");
					for (u32 a = 0x1a5730; a <= 0x1a57c0; a += 4) printf(" %08x", rdi(a));
					// The inflate HOT inner loop: ResourceLoop spends ~all its time at
					// 0x41bf78/0x41bfa8 (Huffman/bit decode). Dump it so we can HLE it
					// like the match-copy (0x41c294) and unblock asset decompression.
					printf("\n[ASMDUMP] inflate@0x41be00:");
					for (u32 a = 0x41be00; a <= 0x41c900; a += 4) printf(" %08x", rdi(a));
					// ResourceLoop's POST-decompression stall: after finishing inflate +
					// registering 42, its cia ends at 0x421734 (r3=0). Dump that region +
					// the 0x420/0x492 code it cycles through, to decode what it polls on.
					printf("\n[ASMDUMP] stall@0x421700:");
					for (u32 a = 0x421700; a <= 0x4217c0; a += 4) printf(" %08x", rdi(a));
					printf("\n[ASMDUMP] stall@0x420400:");
					for (u32 a = 0x420400; a <= 0x420460; a += 4) printf(" %08x", rdi(a));
					printf("\n[ASMDUMP] stall@0x492a00:");
					for (u32 a = 0x492a00; a <= 0x492a40; a += 4) printf(" %08x", rdi(a));
					// 0x3c4c58 = the readiness/"is work ready?" check ResourceLoop polls in
					// its post-decompression spin (called from 0x421730/0x42176c); it keeps
					// returning 0 → ResourceLoop waits forever. Decode what condition it tests.
					printf("\n[ASMDUMP] ready@0x3c4c58:");
					for (u32 a = 0x3c4c58; a <= 0x3c4e80; a += 4) printf(" %08x", rdi(a));
					printf("\n[ASMDUMP] libfs@0xc51100:");
					for (u32 a = 0xc51100; a <= 0xc51300; a += 4) printf(" %08x", rdi(a));
					printf("\n");
				}
			}
		}

#ifdef __EMSCRIPTEN__
// ILMAP: ground-truth map of main's init loop. For each distinct cia main executes
// in [0x1a5730,0x1a57c0], log the LIVE instruction word + r25/r27/r29 once. This
// resolves the hook-anchor confusion (hand-decoded dump offsets were unreliable).
if (false && cia >= 0x1a5730u && cia <= 0x1a57c0u) [[unlikely]] // ILMAP done; disabled
{
	static thread_local u8 s_ilm[40] = {0};
	const u32 idx = (cia - 0x1a5730u) >> 2;
	if (idx < 40 && !s_ilm[idx])
	{
		s_ilm[idx] = 1;
		const auto rdi = [mem_](u32 a){ return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
		printf("[ILMAP] cia=0x%x instr=0x%08x r25=0x%x r27=0x%x r29=0x%x\n", cia, rdi(cia), static_cast<u32>(gpr[25]), static_cast<u32>(gpr[27]), static_cast<u32>(gpr[29]));
	}
}
#endif
#ifdef __EMSCRIPTEN__
// HLE main_thread's giant object-init loop (outer loop @0x1a57a4 (cmpw r27,r29) -> per-element header
// zero routine 0x33f24): it zeroes the 8-byte header of ~7M objects spaced 0xc
// apart, from cursor r29 up to bound r27. main spends ~90% of the pre-flip boot
// HERE at interpreter speed - the dominant bottleneck. Zero the BULK natively and
// leave a margin of objects for the interpreter to finish the exact loop exit.
// NO control-flow change: idempotent zeroing + an updated (stride-aligned) cursor;
// the interpreter's own cmpw(r27,r29) handles termination. 0x33f24 zeroes bytes
// [obj+0..7]; [obj+8..0xb] are untouched (object stride is 0xc).
if (cia == 0x1a57acu) [[unlikely]]
{
			static thread_local u32 s_ilg = 0;
			if ((s_ilg++ & 0xFF) == 0) printf("[INITLOOP] hit#%u cur=0x%x end=0x%x\n", s_ilg, static_cast<u32>(gpr[29]), static_cast<u32>(gpr[27]));
	const u32 cur = static_cast<u32>(gpr[29]);
	const u32 end = static_cast<u32>(gpr[27]);
	if (end > cur + 0x60u && (end - cur) > 0x800u && cur >= 0x20000000u && end < 0x40000000u)
	{
		const u32 k    = (end - cur - 0x600u) / 0x60u; // leave ~128 objs; align to the loop's 0x60 (8-obj) iteration stride
		const u32 stop = cur + k * 0x60u;             // 0x60-aligned so interpreter resumes the loop cleanly
		const u64 wasm_sz  = static_cast<u64>(__builtin_wasm_memory_size(0)) * 65536ull;
		const u64 base_off = reinterpret_cast<u64>(mem_);
		if (stop > cur + 0x60u && base_off + stop + 8 < wasm_sz)
		{
			u8* const g = reinterpret_cast<u8*>(mem_);
			for (u32 a = cur + 0x60u; a < stop; a += 0xcu) // [cur,cur+0x60) already zeroed this iteration
			{
				g[a+0]=0; g[a+1]=0; g[a+2]=0; g[a+3]=0;
				g[a+4]=0; g[a+5]=0; g[a+6]=0; g[a+7]=0;
			}
			gpr[29] = stop - 0x60u; // 0x1a57ac's addi +0x60 makes r29=stop; interpreter does [stop,end)
		}
	}
}
#endif

#ifdef __EMSCRIPTEN__
		// HLE the zlib inflate BIT-REFILL loop (body 0x41bf78-0x41bfa4) — the single
		// hottest spot on EE ResourceLoop (zlib NEEDBITS: read input bytes into the bit
		// accumulator until > 14 bits). Hook the loop's CHECK at 0x41bfa8 and refill
		// natively in one shot; the interpreter then re-reads the (now > 14) bit count,
		// its `ble` falls through, and the loop body is skipped. NO control-flow change,
		// zero desync risk — the resulting state is exactly what the loop would produce.
		// State is a standard zlib inflate_state: +0x18 next_in, +0x1c in_end (one past
		// last input byte), +0x20 hold (bit accumulator), +0x24 bits. Shift amount rises
		// by 8 each iter (matches the interpreter, which reloads r8 only at 0x41bfa8).
		if (cia == 0x41bfa8u) [[unlikely]]
		{
			// INFLATE FAST-PATH: natively run the zlib inffast inner loop (literals +
			// length/distance matches) using the game's own code tables + bit buffer.
			// Decode on temp regs and commit only FULLY-resolved simple symbols; bail
			// to the interpreter on 2nd-level codes, window matches, or EOF — so the
			// state is always left on a symbol boundary (zero desync). Replaces the
			// slow interpreted Huffman decode that throttles EE ResourceLoop's asset
			// decompression (the recurring "main starves on resources" stall).
			const u32 r3v = static_cast<u32>(gpr[3]);
			if (r3v >= 0x10000 && r3v < 0x5fff0000)
			{
				const auto rd = [mem_](u32 a){ return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
				const u64 wasm_sz  = static_cast<u64>(__builtin_wasm_memory_size(0)) * 65536ull;
				const u64 base_off = reinterpret_cast<u64>(mem_);
				u8* const g = reinterpret_cast<u8*>(mem_);
				const u32 lencode  = rd(r3v + 0x08);
				const u32 distcode = rd(r3v + 0x0c);
				const u32 lenbits  = rd(r3v + 0x10);
				const u32 distbits = rd(r3v + 0x14);
				const u32 in_end   = rd(r3v + 0x1c);
				const u32 out_end  = rd(r3v + 0x2c);
				const u32 out_start= rd(r3v + 0x30);
				u32 acc = rd(r3v + 0x20);
				u32 cnt = rd(r3v + 0x24);
				u32 inp = rd(r3v + 0x18);
				u32 nout= rd(r3v + 0x28);
					{ static thread_local int s_idb = 0; if (s_idb < 6) { s_idb++; const u32 le0 = (lencode>=0x10000&&lencode<0x5fff0000)?rd(lencode):0xbadu; printf("[INFDBG] r3=0x%x lenbits=%u distbits=%u lencode=0x%x distcode=0x%x out_start=0x%x out_end=0x%x nout=0x%x cnt=%u lencode0=0x%x\n", r3v, lenbits, distbits, lencode, distcode, out_start, out_end, nout, cnt, le0); } }
					{ static thread_local int s_rej = 0; if (out_end <= out_start && s_rej < 6) { s_rej++; const auto rdj=[mem_](u32 a){ return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); }; printf("[INFREJ] r3=0x%x lb=%u db=%u nout=0x%x:", r3v, lenbits, distbits, nout); for (u32 o = 0; o <= 0x5cu; o += 4) printf(" +%02x=%08x", o, rdj(r3v + o)); printf("\n"); } }
				if (lenbits >= 1 && lenbits <= 12 && distbits >= 1 && distbits <= 12 &&
					lencode >= 0x10000 && lencode < 0x5fff0000 &&
					distcode >= 0x10000 && distcode < 0x5fff0000 &&
					out_end > nout && out_end < 0x5fff0000 && nout >= 0x18000u)
				{
					const u32 lmask = (1u << lenbits) - 1u;
					const u32 dmask = (1u << distbits) - 1u;
					const auto inb = [base_off, wasm_sz](u32 a){ return base_off + u64{a} < wasm_sz; };
					u32 ops = 0;
					static thread_local u64 s_ent = 0, s_zero = 0, s_eob = 0, s_win = 0;
					s_ent++;
					for (;;)
					{
						while (cnt <= 14 && inp < in_end && inb(inp)) { acc += static_cast<u32>(g[inp]) << cnt; inp++; cnt += 8; }
						if (cnt < lenbits) break;
						const u32 lent = lencode + (acc & lmask) * 4u;
						if (!inb(lent + 4u)) break;
						u8  lop = g[lent];
						u8  lcb = g[lent + 1u];
						u32 lval = (static_cast<u32>(g[lent + 2u]) << 8) | g[lent + 3u];
						if (lcb == 0u || lcb > cnt) break;
						if (lop != 0u && !(lop & 0x70u))   // 2nd-level length link -> follow it
						{
							const u32 i2 = lval + ((acc >> lcb) & ((1u << lop) - 1u));
							const u32 le2 = lencode + i2 * 4u;
							if (!inb(le2 + 4u)) break;
							const u8 op2 = g[le2]; const u8 cb2 = g[le2 + 1u];
							const u32 vl2 = (static_cast<u32>(g[le2 + 2u]) << 8) | g[le2 + 3u];
							if (cb2 == 0u || static_cast<u32>(lcb) + cb2 > cnt) break;
							if (op2 != 0u && !(op2 & 16u)) break;   // must resolve to literal/length
							lop = op2; lcb = static_cast<u8>(lcb + cb2); lval = vl2;
						}
						if (lop == 0u) { if (nout >= out_end || !inb(nout)) break; g[nout] = static_cast<u8>(lval); nout++; acc >>= lcb; cnt -= lcb; ops++; continue; }
						if (!(lop & 16u)) { s_eob++; break; }   // EOB / invalid -> interpreter
						u32 tacc = acc >> lcb, tcnt = cnt - lcb, tinp = inp;
						u32 len = lval; const u32 leb = lop & 15u;
						if (leb) { while (tcnt < leb && tinp < in_end && inb(tinp)) { tacc += static_cast<u32>(g[tinp]) << tcnt; tinp++; tcnt += 8; } if (tcnt < leb) break; len += tacc & ((1u << leb) - 1u); tacc >>= leb; tcnt -= leb; }
						while (tcnt <= 14 && tinp < in_end && inb(tinp)) { tacc += static_cast<u32>(g[tinp]) << tcnt; tinp++; tcnt += 8; }
						if (tcnt < distbits) break;
						const u32 dent = distcode + (tacc & dmask) * 4u;
						if (!inb(dent + 4u)) break;
						u8  dop = g[dent];
						u8  dcb = g[dent + 1u];
						u32 dval = (static_cast<u32>(g[dent + 2u]) << 8) | g[dent + 3u];
						if (dcb == 0u || dcb > tcnt) break;
						if (dop != 0u && !(dop & 0x70u))   // 2nd-level distance link -> follow it
						{
							const u32 i2 = dval + ((tacc >> dcb) & ((1u << dop) - 1u));
							const u32 de2 = distcode + i2 * 4u;
							if (!inb(de2 + 4u)) break;
							const u8 op2 = g[de2]; const u8 cb2 = g[de2 + 1u];
							const u32 vl2 = (static_cast<u32>(g[de2 + 2u]) << 8) | g[de2 + 3u];
							if (cb2 == 0u || static_cast<u32>(dcb) + cb2 > tcnt) break;
							if (!(op2 & 16u)) break;   // must resolve to a distance base
							dop = op2; dcb = static_cast<u8>(dcb + cb2); dval = vl2;
						}
						if (!(dop & 16u)) break;
						tacc >>= dcb; tcnt -= dcb;
						u32 dist = dval; const u32 deb = dop & 15u;
						if (deb) { while (tcnt < deb && tinp < in_end && inb(tinp)) { tacc += static_cast<u32>(g[tinp]) << tcnt; tinp++; tcnt += 8; } if (tcnt < deb) break; dist += tacc & ((1u << deb) - 1u); tacc >>= deb; tcnt -= deb; }
						const u32 beg = (out_start >= 0x10000u && out_start <= nout) ? out_start : (nout - 0x8000u);
							if (dist == 0u || dist > (nout - beg)) { s_win++; break; }   // back-ref guard: out_start if sane, else 32KB deflate max
						if (len == 0u || u64{nout} + len > out_end || !inb(nout + len)) break;
						const u32 src = nout - dist;
						for (u32 ci = 0; ci < len; ci++) g[nout + ci] = g[src + ci];
						nout += len; acc = tacc; cnt = tcnt; inp = tinp; ops++;
					}
					if (ops)
					{
						*reinterpret_cast<be_t<u32>*>(mem_ + (u64{r3v} + 0x20)) = acc;
						*reinterpret_cast<be_t<u32>*>(mem_ + (u64{r3v} + 0x24)) = cnt;
						*reinterpret_cast<be_t<u32>*>(mem_ + (u64{r3v} + 0x18)) = inp;
						*reinterpret_cast<be_t<u32>*>(mem_ + (u64{r3v} + 0x28)) = nout;
					}
					else { s_zero++; }
					{ static thread_local u64 s_if = 0; static thread_local u32 s_ic = 0; s_if += ops; if ((++s_ic < 24u) || (s_ic & 0xFFFu) == 0) printf("[INFFAST] ops=%llu last=%u ent=%llu zero=%llu eob=%llu win=%llu\n", static_cast<unsigned long long>(s_if), ops, static_cast<unsigned long long>(s_ent), static_cast<unsigned long long>(s_zero), static_cast<unsigned long long>(s_eob), static_cast<unsigned long long>(s_win)); }
				}
			}
		}
#endif

		// HLE the inflate match-copy loop — the decompression hot path that
		// throttles EE ResourceLoop to ~2.5KB/s (~400 instr/byte) and stops the
		// boot reaching its first flip before the ~8min headless-Chrome ceiling.
		// 0x41c294 is a CTR-counted (mtctr r5 @0x41c290) byte-FORWARD copy from
		// r8(src) → [r3+0x28](dst), exiting via bdz @0x41c2b4 → 0x41c378. We copy
		// the bulk natively but leave the LAST byte to the interpreter so it
		// finalizes every register + the bdz exit with zero desync risk. Forward
		// byte copy == overlap-safe LZ77/RLE semantics (identical to the loop).
		if (cia == 0x41c294) [[unlikely]]
		{
			const u32 n   = static_cast<u32>(ctr);
			const u32 r3v = static_cast<u32>(gpr[3]);
			#ifdef __EMSCRIPTEN__
			// INFSTATE: one-shot dump of the zlib inflate state struct (r3) so we can
			// locate input start/end + output start/end + bit buffer/count, to HLE the
			// whole inflate with native zlib (the real decompression-speed fix).
			{ static thread_local int s_std = 0;
			  if (s_std++ == 0 && r3v >= 0x10000 && r3v < 0x5fff0000) {
			    const auto rds = [mem_](u32 a){ return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
			    printf("[INFSTATE] r3=0x%x:", r3v);
			    for (u32 o = 0; o <= 0x40; o += 4) printf(" +%02x=%08x", o, rds(r3v + o));
			    printf("\n"); } }
			#endif
			// Guard r3v before dereferencing [r3+0x28]; the guest vm region is
			// 0..0x60000000 (1.5 GiB), so anything outside is wild — skip to the
			// interpreter (which masks each access + page-faults safely).
			if (n >= 2 && n <= 0x40000 && r3v >= 0x10000 && r3v < 0x5fff0000)
			{
				const u32 dst = reinterpret_cast<be_t<u32>*>(mem_ + (u64{r3v} + 0x28))->value();
				const u32 src = static_cast<u32>(gpr[8]);
				// Only HLE when the WHOLE copy lies inside the guest vm. A
				// malformed back-ref (src = dst - distance) can underflow to a
				// wild address; the byte-loop interpreter tolerates that via
				// 32-bit masking + vm fault handling, but our raw host copy must
				// not run off the wasm heap. Out-of-range → fall back.
				bool can_hle = (dst >= 0x10000 && src >= 0x10000 &&
				    u64{dst} + n <= 0x60000000 && u64{src} + n <= 0x60000000);
#ifdef __EMSCRIPTEN__
				// Growable (Dyna) wasm heap: 0x60000000 above is the RESERVED guest-vm
				// size, but the wasm linear memory may not have GROWN that far yet this
				// early in boot. A raw host copy to an un-grown high address traps
				// ("memory access out of bounds") and kills EE ResourceLoop — whereas the
				// interpreter's vm:: access page-faults + grows on demand. So also require
				// the WHOLE copy to lie within the CURRENT wasm memory; else fall back to
				// the (safe, self-growing) interpreter. Fixes the flaky early-boot OOB
				// that killed ResourceLoop mid-decompression (stackwalk155).
				if (can_hle)
				{
					const u64 base_off  = reinterpret_cast<u64>(mem_);
					const u64 wasm_size = static_cast<u64>(__builtin_wasm_memory_size(0)) * 65536ull;
					if (base_off + u64{dst} + n > wasm_size || base_off + u64{src} + n > wasm_size)
						can_hle = false;
				}
#endif
				if (can_hle)
				{
					const u32 bulk = n - 1;
					u8* const gmem = reinterpret_cast<u8*>(mem_);
					for (u32 i = 0; i < bulk; i++)
						gmem[u64{dst} + i] = gmem[u64{src} + i];
					*reinterpret_cast<be_t<u32>*>(mem_ + (u64{r3v} + 0x28)) = dst + bulk;
					gpr[8] = src + bulk;
					ctr = 1; // interpreter copies the final byte, then bdz exits cleanly
					// INFLATE progress proxy: accumulate match-copy bytes + log throttled,
					// to gauge whether asset data is a few MB (incremental-HLE/patience ok)
					// or 10s of MB (need native zlib). Under-counts (literals excluded).
					static thread_local u64 s_infl = 0; static thread_local u32 s_infn = 0;
					s_infl += bulk;
					if ((++s_infn & 0x3FFF) == 0)
						printf("[INFLATE] matchcopies=%u matchbytes=%lluKiB\n", s_infn, s_infl >> 10);
				}
			}
		}

		// INFLATE-DUMP / STREAM-WATCH / INFLATE-TRACE diagnostics removed: the
		// match-copy is now HLE'd (above) so the boot races ~30x faster, and
		// these high-rate per-instruction printf logs flooded the CDP console
		// (~20 msgs/sec), triggering an early headless-renderer disconnect.
		// Decompression confirmed finite-but-slow; no longer need to trace it.

		// SLOT-WATCH: the game's master singleton pointer G lives in EBOOT data
		// slot (eboot_toc - 0x7780) == 0x59fcc8. The CHAIN diag proved G=*slot is
		// a VALID heap ptr (0x9714f0) but G's object is entirely ZEROED -> it was
		// allocated yet never constructed (vtable G[0]=0, manager G[0x10]=0). The
		// open question is WHY the ctor never ran. Answer it empirically in ONE
		// run by logging, cross-thread and one-shot:
		//   (a) the exact STORE SITE that writes G into the slot, with a wide code
		//       window so we can read whether a `bl <ctor>` is emitted around it;
		//   (b) whether G[0] (vtable) or G[0x10] (manager) EVER become non-zero
		//       later (=> ctor does run, just late/raced) or stay zero (=> the ctor
		//       call is genuinely never reached / writes elsewhere).
		// Cheap: 1-3 absolute memory loads per instruction until all flags latch.
		// DISABLED (s_diag_g=false): the G-singleton ctor question is RESOLVED
		// (G0-WRITE fired — G is constructed; the boot runs far past it). In
		// practice this block did ~6 atomic_t loads + an rdw() + a ring write on
		// EVERY PPU instruction, which on wasm/SharedArrayBuffer throttled the
		// interpreter to ~167K instr/s — far too slow for the resource-load boot
		// (EE ResourceLoop streaming 200+ zlib assets) to reach the first flip in
		// a capture window. Gating it off (one thread-local bool test) restores
		// interpreter speed. Set s_diag_g=true to re-arm the G investigation.
		static thread_local bool s_diag_g = false;
		if (eboot_toc && s_diag_g) [[unlikely]]
		{
			const u32 kSlot = eboot_toc - 0x7780; // 0x59fcc8
			static atomic_t<u32> s_slot_done{0};
			static atomic_t<u32> s_G{0};
			static atomic_t<u32> s_vt_done{0};
			static atomic_t<u32> s_mgr_done{0};
			const auto rdw = [mem_](u32 a) -> u32 {
				return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value();
			};
			// SPIN-WATCH: post-init the game loop busy-spins at 0x41c294
			// (lwz r11,0x28(r3)) polling [r3+0x28], never reaching a GCM flip.
			// Identify the polled object (r3), its vtable + the two polled
			// fields, the thread, and whether the value EVER changes. Log the
			// first few hits then sparsely so we can see motion vs deadlock.
			if (cia == 0x41c294) [[unlikely]]
			{
				static atomic_t<u32> s_spin{0};
				static atomic_t<u32> s_first28{0};
				const u32 n = s_spin.fetch_add(1);
				const u32 v28 = rdw(static_cast<u32>(gpr[3]) + 0x28);
				if (n == 0) s_first28 = v28;
				if (n < 4 || (n & 0x3FFFFF) == 0)
				{
					const u32 r3 = static_cast<u32>(gpr[3]);
					// Dump the inflate-state struct header (r3+0x00..0x3c) so we
					// can find the monotonically-growing total_out / next_out
					// field. If one field climbs steadily across samples the
					// decompression is FINITE-but-slow; if all are frozen it is
					// genuinely stuck.
					printf("[SPIN-STATE] n=%u r3=0x%x | +00=%08x +04=%08x +08=%08x +0c=%08x +10=%08x +14=%08x +18=%08x +1c=%08x +20=%08x +24=%08x +28=%08x +2c=%08x +30=%08x +34=%08x +38=%08x +3c=%08x\n",
						n, r3,
						rdw(r3 + 0x00), rdw(r3 + 0x04), rdw(r3 + 0x08), rdw(r3 + 0x0c),
						rdw(r3 + 0x10), rdw(r3 + 0x14), rdw(r3 + 0x18), rdw(r3 + 0x1c),
						rdw(r3 + 0x20), rdw(r3 + 0x24), rdw(r3 + 0x28), rdw(r3 + 0x2c),
						rdw(r3 + 0x30), rdw(r3 + 0x34), rdw(r3 + 0x38), rdw(r3 + 0x3c));
				}
			}
			// Ring buffer of recent block-entry cias on THIS thread. The slot
			// store lands BETWEEN block boundaries (we saw "first seen" land on
			// a liblv2 trampoline @0xbe0000, NOT the real store), so to find
			// the store site we replay the call path that led up to it.
			static thread_local u32 s_ring[32] = {0};
			static thread_local u32 s_ring_i = 0;
			s_ring[s_ring_i & 31] = cia;
			s_ring_i++;
			// MAIN-TRACE: main() (0x141d84..0x142150) runs only on main_thread.
			// The G[0] sub-manager is constructed by one of main's early `bl`
			// calls; if an HLE call returns the wrong status, main takes an error
			// branch and SKIPS that construction, leaving G[0]=0 (the crash). Log
			// main's own block path + r3 (= prior call's return value at a return
			// block) so we can see exactly which branch diverges. One-shot, capped.
			if (cia >= 0x141d84 && cia < 0x143000) [[unlikely]]
			{
				static thread_local u32 s_mt = 0;
				if (s_mt < 400)
				{
					s_mt++;
					printf("[MAIN-TRACE] cia=0x%x r3=%llx r4=%llx r5=%llx r6=%llx lr=0x%llx\n",
						cia, gpr[3], gpr[4], gpr[5], gpr[6], static_cast<unsigned long long>(lr));
				}
			}
			// GLOBAL G[0] WRITE-WATCH: is *(0x9714f0)=G[0] EVER written non-zero by
			// ANY thread across the WHOLE boot? The 56-block AFTER-STORE window was
			// too narrow. If this never prints, the sub-manager's construction is
			// genuinely never executed (definitive); if it prints, we learn the
			// writer's cia/thread (=> it writes late or to a racing address).
			{
				static atomic_t<u32> s_g0_seen{0};
				if (!s_g0_seen.load()) [[unlikely]]
				{
					const u32 g0 = rdw(0x9714f0);
					if (g0 && s_g0_seen.exchange(1) == 0)
					{
						const auto spn = ppu_tname.load();
						printf("[G0-WRITE] *(0x9714f0)=0x%x first non-zero at cia=0x%x lr=0x%llx thread='%s'\n",
							g0, cia, static_cast<unsigned long long>(lr),
							(spn && !spn->empty()) ? spn->c_str() : "<unnamed>");
					}
				}
			}
			// RWLOCK-EFAULT live args: dump GPRs at the failing
			// sys_rwlock_create call site (0x42f92c). r3=rw_lock_id (0x971664),
			// r4=attr (logged as 0 → EFAULT). If r4 should be a stack ptr
			// (r1+N) this is an arg-setup bug; if r4 is genuinely 0 it's by
			// design. Also dump r1 to confirm the stack pointer is sane.
			{
				static atomic_t<u32> s_rwl_seen{0};
				if (cia == 0x42f92c && s_rwl_seen.exchange(1) == 0)
				{
					printf("[RWL-ARGS] cia=0x%x r1=0x%llx r3=0x%llx r4=0x%llx r5=0x%llx r2=0x%llx lr=0x%llx\n",
						cia,
						static_cast<unsigned long long>(gpr[1]),
						static_cast<unsigned long long>(gpr[3]),
						static_cast<unsigned long long>(gpr[4]),
						static_cast<unsigned long long>(gpr[5]),
						static_cast<unsigned long long>(gpr[2]),
						static_cast<unsigned long long>(lr));
				}
			}
			// AFTER-STORE forward trace: once the slot is written, dump the
			// next N block entries on the storing thread. Decisive question:
			// does a ctor `bl` follow the alloc+store (G should fill) or does
			// the game proceed with G still zeroed?
			static thread_local u32 s_after = 0; // remaining after-trace blocks
			if (s_after)
			{
				const u32 g = s_G.load();
				printf("[AFTER-STORE] cia=0x%x lr=0x%llx r3=%llx r4=%llx | G[0]=%08x G[0x10]=%08x\n",
					cia, static_cast<unsigned long long>(lr), gpr[3], gpr[4],
					rdw(g), rdw(g + 0x10));
				s_after--;
			}
			const u32 gnow = rdw(kSlot);
			if (gnow && !s_slot_done.load() && s_slot_done.exchange(1) == 0)
			{
				s_G = gnow;
				s_after = 56; // begin after-trace on this thread next iteration
				const auto sp_name = ppu_tname.load();
				const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
				printf("[SLOT-WATCH store] thread='%s' slot=0x%x -> G=0x%x first seen at cia=0x%x lr=0x%llx\n",
					nm, kSlot, gnow, cia, static_cast<unsigned long long>(lr));
				printf("  r3=%llx r4=%llx r28=%llx r29=%llx r30=%llx r31=%llx\n",
					gpr[3], gpr[4], gpr[28], gpr[29], gpr[30], gpr[31]);
				printf("  G[0x00..0x20]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
					rdw(gnow), rdw(gnow + 4), rdw(gnow + 8), rdw(gnow + 0xc),
					rdw(gnow + 0x10), rdw(gnow + 0x14), rdw(gnow + 0x18), rdw(gnow + 0x1c));
				// Recent block path (oldest -> newest); s_ring_i points at the
				// oldest slot. The most-recent EBOOT-range entry is the likely
				// store block.
				printf("  ring (oldest->newest):\n");
				for (u32 k = 0; k < 32; k++)
					printf("    r[%u]=0x%x\n", k, s_ring[(s_ring_i + k) & 31]);
				// Code windows for the last 4 distinct recent blocks so the
				// alloc/store/ctor emission is disassemblable.
				for (u32 k = 1; k <= 4; k++)
				{
					const u32 c = s_ring[(s_ring_i - k) & 31] & ~3u;
					if (!c) continue;
					printf("  win[-%u] @0x%x:", k, c);
					for (u32 a = c - 0x20; a <= c + 0x20; a += 4)
						printf(" %08x", rdw(a));
					printf("\n");
				}
			}
			// SLOT-CHANGE: does the slot ever get OVERWRITTEN after load? The
			// relocated value (0x9714f0, a zeroed .bss placeholder) might be
			// meant to be replaced by a heap `new Manager()`. If we never see a
			// change, G is a static singleton filled in-place (and its in-place
			// init is what's missing); if we DO see a change, a skipped `new`
			// is the bug.
			{
				static atomic_t<u32> s_slot_last{0xffffffffu};
				const u32 prev = s_slot_last.load();
				if (prev != gnow)
				{
					if (prev != 0xffffffffu)
					{
						const auto sp_name = ppu_tname.load();
						const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
						printf("[SLOT-CHANGE] thread='%s' slot 0x%x: 0x%x -> 0x%x at cia=0x%x lr=0x%llx\n",
							nm, kSlot, prev, gnow, cia, static_cast<unsigned long long>(lr));
					}
					s_slot_last = gnow;
				}
			}
			// G-AS-THIS: catch any method/ctor invoked with this==G. The
			// dispatcher reads G via TOC (r3 is unrelated there), so a hit here
			// means the game DID try to construct/use G as an object. If this
			// never fires before the crash, G is never touched as an object ->
			// its construction was gated out entirely (HLE result?).
			{
				static atomic_t<u32> s_gthis{0};
				const u32 gg = s_G.load();
				if (gg && static_cast<u32>(gpr[3]) == gg && s_gthis.load() < 16 && s_gthis.exchange(s_gthis.load() + 1) < 16)
				{
					const auto sp_name = ppu_tname.load();
					const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
					printf("[G-THIS] thread='%s' cia=0x%x lr=0x%llx r3==G=0x%x | G[0]=%08x\n",
						nm, cia, static_cast<unsigned long long>(lr), gg, rdw(gg));
				}
			}
			const u32 g = s_G.load();
			if (g && !s_vt_done.load() && rdw(g) != 0 && s_vt_done.exchange(1) == 0)
			{
				const auto sp_name = ppu_tname.load();
				const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
				printf("[SLOT-WATCH vtable] thread='%s' G[0]=0x%x became non-zero at cia=0x%x lr=0x%llx\n",
					nm, rdw(g), cia, static_cast<unsigned long long>(lr));
				const u32 c = cia & ~3u;
				for (u32 a = c - 0x40; a <= c + 0x10; a += 0x10)
					printf("  vw @0x%x: %08x %08x %08x %08x\n", a, rdw(a), rdw(a + 4), rdw(a + 8), rdw(a + 0xc));
			}
			if (g && !s_mgr_done.load() && rdw(g + 0x10) != 0 && s_mgr_done.exchange(1) == 0)
			{
				const auto sp_name = ppu_tname.load();
				const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
				printf("[SLOT-WATCH manager] thread='%s' G[0x10]=0x%x became non-zero at cia=0x%x lr=0x%llx\n",
					nm, rdw(g + 0x10), cia, static_cast<unsigned long long>(lr));
				const u32 c = cia & ~3u;
				for (u32 a = c - 0x40; a <= c + 0x10; a += 0x10)
					printf("  mw @0x%x: %08x %08x %08x %08x\n", a, rdw(a), rdw(a + 4), rdw(a + 8), rdw(a + 0xc));
			}

			// EBOOT-ENTRY WATCH: G@0x9714f0 lives in EBOOT .bss and must be
			// filled by a C++ global constructor that the EBOOT CRT invokes.
			// The TOC-fix first fires at cia=0x103ac but e_entry is 0x10230 — if
			// liblv2 jumps PAST the CRT's .init_array/ctor loop, every static
			// singleton stays zeroed (exactly our symptom). Log each thread's
			// first EBOOT instr (cia + caller lr) and dump the e_entry CRT code
			// once so the ctor-invocation sequence can be disassembled.
			static thread_local bool s_tl_eboot_entry = false;
			if (cia >= eboot_beg && cia < eboot_end && !s_tl_eboot_entry)
			{
				s_tl_eboot_entry = true;
				const auto sp_name = ppu_tname.load();
				const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
				printf("[EBOOT-ENTRY] thread='%s' first EBOOT instr cia=0x%x lr=0x%llx (e_entry=0x10230)\n",
					nm, cia, static_cast<unsigned long long>(lr));
				static atomic_t<u32> s_ee_dump{0};
				if (s_ee_dump.exchange(1) == 0)
				{
					for (u32 a = 0x10230; a <= 0x10230 + 0x180; a += 0x10)
						printf("  ee @0x%x: %08x %08x %08x %08x\n", a, rdw(a), rdw(a + 4), rdw(a + 8), rdw(a + 0xc));
				}
			}

			// CTOR-WALK probe: __do_global_ctors_aux @0x3bf8a8 is the GCC global
			// constructor walker that _init (0x10200) calls (bl @0x10470, BEFORE
			// main @0x104d4). The ctor list at TOC[0x515c]=0x59006c is POPULATED
			// (26 entries, descriptors 0x593df0..0x59ebf0 with VALID OPDs e=0x10b08
			// / 0x4916d8), yet G's vtable stays 0. The static interpreter dispatches
			// at BLOCK boundaries, so the mid-block bctrl (0x3bf8e8) is never a loop
			// `cia` — probe the loop-BODY entry 0x3bf8d0 (gpr[9]=current OPD ptr),
			// the bctrl RETURN 0x3bf8ec (a ctor returned), and EXIT 0x3bf900. This
			// reveals how many ctors are dispatched, which entries, and whether each
			// one returns (a hang/diverge in ctor #k would stop the walk at k).
			static thread_local u32 s_cw_body = 0; // loop-body entries (dispatches)
			static thread_local u32 s_cw_ret  = 0; // ctor returns
			if (cia == 0x3bf8a8) [[unlikely]]
			{
				static thread_local bool s_cw_seen = false;
				if (!s_cw_seen)
				{
					s_cw_seen = true;
					const auto sp_name = ppu_tname.load();
					const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
					printf("[CTORW] thread='%s' ENTERED ctors_aux r2=0x%llx TOC[0x515c]=0x%x OPD@0x59ebf0.e=0x%x OPD@0x593df0.e=0x%x\n",
						nm, static_cast<unsigned long long>(gpr[2]), rdw(eboot_toc + 0x515c), rdw(0x59ebf0), rdw(0x593df0));
					// FULL ctor-list dump — tests the "truncated .ctors / ignored
					// .init_array" hypothesis directly. GCC __CTOR_LIST__ layout:
					// [0]=count (or 0xffffffff = "scan to NULL"), [1..n]=OPD ptrs,
					// [n+1]=0 terminator. Walk it raw; for each entry print the OPD
					// ptr and its function entry word (OPD[0]). If the writer of
					// G[0] (0x9714f0) is a ctor, its code must appear in this list.
					const u32 clp = rdw(eboot_toc + 0x515c);
					printf("[CTORW-LIST] base=0x%x count_word=0x%x\n", clp, rdw(clp));
					for (u32 i = 1; i <= 48; i++)
					{
						const u32 opd = rdw(clp + i * 4);
						if (opd == 0) { printf("[CTORW-LIST] #%u = 0 (terminator)\n", i); break; }
						printf("[CTORW-LIST] #%u opd=0x%x entry=0x%x\n", i, opd, rdw(opd));
					}
				}
			}
			else if (cia == 0x3bf8d0) [[unlikely]]
			{
				// gpr[9] = current ctor list entry (OPD ptr), verified != -1.
				if (s_cw_body < 2)
					printf("[CTORW] body #%u desc=0x%llx entry=0x%x\n",
						s_cw_body, static_cast<unsigned long long>(gpr[9]), rdw(static_cast<u32>(gpr[9])));
				s_cw_body++;
			}
			else if (cia == 0x3bf8ec) [[unlikely]]
			{
				s_cw_ret++;
			}
			else if (cia == 0x10474) [[unlikely]]
			{
				// 0x10474 = the instruction in _init (0x10200) right AFTER the
				// `bl __do_global_ctors_aux` at 0x10470 — i.e. the point control
				// returns to once the WHOLE ctor walk is done. This is a real
				// block-entry (call-return), so unlike 0x3bf900 it actually fires.
				// If returned < dispatched, a ctor diverged / broke out early
				// (band-aid) WITHOUT returning => it never finished registering
				// its handler => the null-OPD dispatch crash downstream.
				static thread_local bool s_cw_exit = false;
				if (!s_cw_exit && s_cw_body > 0)
				{
					s_cw_exit = true;
					// Re-read G[0] (*0x9714f0) the instant the WHOLE ctor walk
					// returns. If it's still 0 here, NO global ctor wrote it →
					// G's sub-object is filled by a runtime/lazy path (HLE-gated),
					// not by the ctor walk → kills hypothesis (a), leaves (b).
					printf("[CTORW] RETURN to _init@0x10474: dispatched=%u returned=%u G[0]=*(0x9714f0)=0x%x\n",
						s_cw_body, s_cw_ret, rdw(0x9714f0));
				}
			}

			// WORKER-SPIN probe: WorkerThread (func 0x420fac) ends up spinning at
			// 0x3c4ae0 → 0x3c4af8 → 0x3c4b04 (TOC-FIX cycles those exact 3 blocks),
			// calling into liblv2 each iteration — a poll/wait loop. It never
			// reaches the code that would create G[0] (the all-thread [G0-WRITE]
			// watch stays silent for ~12s), so boot is a multi-thread deadlock:
			// main races to the G[0] dispatch and crashes while WorkerThread waits
			// here forever. One-shot dump of the loop body + GPRs to identify the
			// poll target (which flag/event/var it waits on → who should set it).
			if (cia == 0x3c4ae0) [[unlikely]]
			{
				static thread_local bool s_ws = false;
				if (!s_ws)
				{
					s_ws = true;
					const auto spn = ppu_tname.load();
					printf("[WORKER-SPIN] thread='%s' @0x3c4ae0 loop body:\n",
						(spn && !spn->empty()) ? spn->c_str() : "<unnamed>");
					for (u32 a = 0x3c4ac0; a <= 0x3c4b10; a += 4)
						printf("  i @0x%x: %08x\n", a, rdw(a));
					printf("  GPR r3=%llx r4=%llx r5=%llx r6=%llx r7=%llx r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx\n",
						static_cast<unsigned long long>(gpr[3]), static_cast<unsigned long long>(gpr[4]), static_cast<unsigned long long>(gpr[5]),
						static_cast<unsigned long long>(gpr[6]), static_cast<unsigned long long>(gpr[7]), static_cast<unsigned long long>(gpr[8]),
						static_cast<unsigned long long>(gpr[9]), static_cast<unsigned long long>(gpr[10]), static_cast<unsigned long long>(gpr[11]),
						static_cast<unsigned long long>(gpr[12]));
					printf("  GPR r24=%llx r25=%llx r26=%llx r27=%llx r28=%llx r29=%llx r30=%llx r31=%llx\n",
						static_cast<unsigned long long>(gpr[24]), static_cast<unsigned long long>(gpr[25]), static_cast<unsigned long long>(gpr[26]),
						static_cast<unsigned long long>(gpr[27]), static_cast<unsigned long long>(gpr[28]), static_cast<unsigned long long>(gpr[29]),
						static_cast<unsigned long long>(gpr[30]), static_cast<unsigned long long>(gpr[31]));
					// Dump the 3 functions the poll loop calls (bl 0x4cf508 = poll,
					// bl 0x4cf588 = sleep(r4=ms*1000), bl 0x4cf528 = poll). Reveals
					// what memory/flag the loop tests (the condition that never
					// becomes true) and whether they thunk to liblv2/HLE imports.
					const u32 pollfns[3] = {0x4cf508u, 0x4cf528u, 0x4cf588u};
					for (u32 k = 0; k < 3; k++)
					{
						const u32 fbase = pollfns[k];
						printf("  [POLL-FN] @0x%x:\n", fbase);
						for (u32 a = fbase; a < fbase + 0x40; a += 4)
							printf("    %08x: %08x\n", a, rdw(a));
					}
				}
			}

			// MAIN-TRACE probe: main() @0x141d84 runs a long bl-sequence of init
			// calls; the crash is a null OBJECT pointer (r27=0) used after one of
			// these returns. The block-granularity interpreter only surfaces
			// bl-RETURN addresses as dispatch `cia`, so logging every block-entry
			// within main's body [0x141d84..0x142150] captures r3 = the value the
			// just-returned callee produced. The first init that returns 0 / null /
			// an error code is the root: main bails and the cleanup/lookup path
			// dereferences the unconstructed .bss singleton.
			if (cia >= 0x141d84 && cia <= 0x142150) [[unlikely]]
			{
				static thread_local u32 s_mt = 0;
				if (s_mt < 80)
					printf("[MAIN] #%u cia=0x%x lr=0x%llx r3=0x%llx r4=0x%llx r27=0x%llx r29=0x%llx r30=0x%llx r31=0x%llx\n",
						s_mt, cia,
						static_cast<unsigned long long>(lr),
						static_cast<unsigned long long>(gpr[3]),
						static_cast<unsigned long long>(gpr[4]),
						static_cast<unsigned long long>(gpr[27]),
						static_cast<unsigned long long>(gpr[29]),
						static_cast<unsigned long long>(gpr[30]),
						static_cast<unsigned long long>(gpr[31]));
				s_mt++;
			}
		}
#endif

		// Execute instruction (may be step; execute only one instruction if state)
		const auto op = reinterpret_cast<be_t<u32>*>(mem_ + u64{cia});
		const auto fn = reinterpret_cast<ppu_intrp_func*>(cache + u64{cia} * 2);
#if defined(__EMSCRIPTEN__)
		// NULL-TOC CASCADE FIX (see g_eboot_toc definition near top of file).
		// main_thread hands off from LLE liblv2 into the EBOOT, but liblv2 calls
		// the EBOOT _start WITHOUT loading the EBOOT TOC, so the game's C-runtime
		// and C++ global constructors run with the WRONG r2 (either 0 or liblv2's
		// own TOC 0xc08ab0). Wrong r2 => every TOC-relative global read is garbage
		// => static initialization leaves singletons zeroed => later virtual /
		// callback calls fetch a null function pointer (cia=0 null-fn traps) and
		// worker threads dereference null objects (memory-access OOB).
		// In PPC64 ELFv1 r2 is a per-module CONSTANT: EVERY instruction in the
		// EBOOT module must execute with r2 == the EBOOT TOC (the caller sets it
		// via the OPD before any cross-module call; intra-module flow never
		// changes it). So we enforce that invariant directly: whenever EBOOT-range
		// code is about to run with r2 != eboot_toc, restore it. This is correct
		// (not a band-aid) and also self-heals stale on-stack saved-TOC values,
		// because the next `std r2,0x28(r1)` then saves the corrected TOC.
		if (eboot_toc && cia >= eboot_beg && cia < eboot_end && gpr[2] != eboot_toc) [[unlikely]]
		{
			const u32 bad_r2 = static_cast<u32>(gpr[2]);
			gpr[2] = eboot_toc;
			static thread_local bool s_entered = false;
			static thread_local u32 s_tocfix = 0;
			const auto sp_name = ppu_tname.load();
			const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
			if (!s_entered)
			{
				s_entered = true;
				printf("[TOC-ENTER] thread='%s' first wrong-r2 EBOOT instr: cia=0x%x had r2=0x%x -> 0x%x\n",
					nm, cia, bad_r2, eboot_toc);
			}
			if (s_tocfix++ < 6)
			{
				printf("[TOC-FIX #%u] thread='%s' r2 0x%x->0x%x at cia=0x%x lr=0x%llx\n",
					s_tocfix, nm, bad_r2, eboot_toc, cia, static_cast<unsigned long long>(lr));
			}
		}
		// === MAIN-RET disasm (stackwalk49) ===
		// With online init skipped, 0x409a90 returns r3=0 to main@0x142148 and main
		// takes a cleanup/exit path (cellPadEnd/sys_config_stop) instead of proceeding
		// to cellGcmInit. Dump main's post-0x409a90 check ONCE to find the success
		// condition (what r3/value avoids the bail), so we can drive main toward
		// rendering. Fires even without a crash (the one-shot DIAG needs a null-fn).
		if (cia == 0x142148) [[unlikely]]
		{
			static thread_local bool s_mret = false;
			if (!s_mret)
			{
				s_mret = true;
				const auto rdm = [mem_](u32 a) -> u32 { return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value(); };
				printf("[MAIN-RET] 0x409a90 -> r3=0x%llx ; main disasm 0x142144..0x142284:\n",
					static_cast<unsigned long long>(gpr[3]));
				for (u32 p = 0x142144; p < 0x142400; p += 0x10)
					printf("  [MAIN-DIS] @0x%x: %08x %08x %08x %08x\n", p, rdm(p), rdm(p + 4), rdm(p + 8), rdm(p + 0xc));
			}
			// DRIVE SUCCESS PATH (stackwalk50): main does `if ((r3 & 0xff) != 0) goto
			// 0x142264 (success → game/cellGcmInit)` else falls through to cleanup+exit.
			// With ONLINE-SKIP, 0x409a90 returns 0 → main bails. Force r3=1 so main takes
			// the success branch. EXPERIMENT to test if real rendering (GCM) is reachable
			// now that the rest of 0x409a90's container init DID run (unlike the whole-skip
			// which crashed at 0x2d65b8). Any pixels come from real GCM, not faked.
			if (gpr[3] == 0)
			{
				printf("[MAIN-FORCE] 0x409a90 r3=0 -> forcing r3=1 (take success path 0x142264)\n");
				gpr[3] = 1;
			}
		}
		// === ONLINE-SKIP (stackwalk48) — reachability experiment ===
		// main@0x142144 -> 0x409a90 -> bctrl@0x409bf8 -> 0x1d6e90 -> 0x65614 ->
		// 0x1be60 (np init, a0=0x974b2c) -> ... -> fn7f (reads unconstructed G[0]=0).
		// The whole np/online init subtree depends on managers the type-registration
		// never built. Skip JUST this subtree (entry 0x1d6e90, guarded to the
		// bctrl@0x409bf8 caller so it's surgical) and return success, letting the REST
		// of 0x409a90 finish its real container init and main proceed toward
		// cellGcmInit. NOT faking pixels — stubbing a broken subsystem to test whether
		// real rendering is reachable. Self-disables once G[0] is built.
		if (cia == 0x1d6e90 && (static_cast<u32>(lr) & ~3u) == 0x409bfc &&
			reinterpret_cast<be_t<u32>*>(mem_ + u64{0x9714f0})->value() == 0) [[unlikely]]
		{
			printf("[ONLINE-SKIP] skip 0x1d6e90 online/np init (G[0]=0) -> r3=0 ret to 0x%x\n",
				static_cast<u32>(lr) & ~3u);
			gpr[3] = 0;
			cia = static_cast<u32>(lr) & ~3u;
			continue;
		}
		// === POLL-BYPASS (v3, entry-skip) — robust resource-registration-wall bypass ===
			// main's "is resource registered" fn (entry 0x2761c) is a 4-way unrolled
			// hash-bucket id search; its id-vs-target (r7) compares are scattered across
			// 0x27730/0x2777c/0x277a8/etc., so patching individual compare sites (v2) missed
			// the bucket slots main actually used (0x277b4/0x27788). ~7 ids (0x1ef/0x131/
			// 0x223/0x228/0x233/0x23f/0x26b) never register, so main's poll loops forever and
			// never flips. Skip the WHOLE fn at its entry for ONLY those stuck descriptors
			// (r3=descriptor; target id = halfword @[r3+0x6c]) and return "found" (r3=1, and
			// [r3+0x15]=1 like the real found-path) via the established cia=lr blr-emulate so
			// main advances past the resource wall toward the flip. Gated to main + valid ptr.
			if (cia == 0x2761cu && this->id == 0x1000000u) [[unlikely]]
			{
				const u32 descp = static_cast<u32>(gpr[3]);
				if (descp >= 0x10000u && descp < 0x5ff00000u)
				{
					const u16 tgtp = reinterpret_cast<be_t<u16>*>(mem_ + (u64{descp} + 0x6cu))->value();
					if (tgtp == 0x1efu || tgtp == 0x131u || tgtp == 0x223u || tgtp == 0x228u || tgtp == 0x233u || tgtp == 0x23fu || tgtp == 0x26bu)
					{
						*reinterpret_cast<u8*>(mem_ + (u64{descp} + 0x15u)) = 1u;
						gpr[3] = 1;
						static thread_local u32 s_byp3 = 0;
						if ((s_byp3++ & 0x3Fu) == 0u) printf("[POLLBYPASS3] entry-skip tgt=0x%x desc=0x%x lr=0x%x n=%u\n", static_cast<unsigned>(tgtp), static_cast<unsigned>(descp), static_cast<unsigned>(lr), s_byp3);
						cia = static_cast<u32>(lr) & ~3u;
						continue;
					}
				}
			}
			// === POLL-BYPASS (v4, jump-to-found) — covers ALL stuck ids at the shared loop head ===
			// v3 (entry-skip @0x2761c) only caught 0x131; the other stuck ids reach the search
			// via other entries but ALL hit the per-iteration loop head 0x27700 (RESSCAN proves
			// it). Read the target id directly from [r31+0x6c] (r31=descriptor, exactly as
			// RESSCAN does — r7 is NOT the live target in the hash-bucket loop). When it is one
			// of the stuck ids, jump straight to the function's OWN found-handler 0x2766c (li r6,1;
			// stb r6,0x15(r31); li r3,1; b epilogue) — the prologue already ran, so the epilogue
			// tears the frame down cleanly and returns "found". One point covers all 7.
			if (cia == 0x27700u && this->id == 0x1000000u) [[unlikely]]
			{
				const u32 r31v = static_cast<u32>(gpr[31]);
				const u16 t7p = (r31v >= 0x10000u && r31v < 0x5ff00000u) ? reinterpret_cast<be_t<u16>*>(mem_ + (u64{r31v} + 0x6cu))->value() : static_cast<u16>(0);
				if (t7p == 0x1efu || t7p == 0x131u || t7p == 0x223u || t7p == 0x228u || t7p == 0x233u || t7p == 0x23fu || t7p == 0x26bu)
				{
					static thread_local u32 s_byp4 = 0;
					if ((s_byp4++ & 0xFFu) == 0u) printf("[POLLBYPASS4] jump-to-found tgt=0x%x 0x27700->0x2766c n=%u\n", static_cast<unsigned>(t7p), s_byp4);
					cia = 0x2766cu;
					continue;
				}
			}
			// === fn7f BYPASS (stackwalk32) — empirical: skip the G[0] consumer ===
		// fn7f @0x7f8e4 unconditionally derefs G[0]=*(0x9714f0) (lwz r5,0(r6);
		// lwz r28,0x10(r5) @0x7f944, NO null guard). G[0] is NEVER constructed in
		// this limping boot — every static writer scan (G-SET/G-CTOR/G-ABS) = 0
		// AND the runtime G0-WRITE poll never fires. So fn7f always cascades into a
		// null vtable call (null-fn @0x7f9c8) and main wedges after 3 band-aids.
		// Rather than band-aid the downstream null calls, skip fn7f at its entry
		// (0x7f8e4 is a reliable bl-target block start) and return r3=0 to its
		// caller (lr=0x1d6fbc, the per-node registry dispatch). GOAL: let main
		// proceed PAST the 0x409a90 type-registry walk toward cellGcmInit so the
		// RSX actually flips real pixels. Guarded on G[0]==0 so it self-disables if
		// G[0] ever gets built. If main still wedges right after, escalate to
		// skipping the whole 0x409a90 walk (return r3=1 to main@0x142148).
		if (cia == 0x7f8e4 &&
			reinterpret_cast<be_t<u32>*>(mem_ + u64{0x9714f0})->value() == 0) [[unlikely]]
		{
			static thread_local u32 s_fn7f_skip = 0;
			if (s_fn7f_skip++ < 8)
			{
				const auto sp_name = ppu_tname.load();
				const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
				printf("[fn7f BYPASS #%u] thread='%s' skip 0x7f8e4 (G[0]=0) -> r3=0 ret to lr=0x%llx\n",
					s_fn7f_skip, nm, static_cast<unsigned long long>(lr));
			}
			gpr[3] = 0;
			cia = static_cast<u32>(lr) & ~3u;
			continue;
		}
		// === 0x409a90 WHOLE-INIT SKIP (stackwalk33) — empirical whack-a-mole exit ===
		// fn7f was only ONE of many G[0]/manager consumers reached from main's big
		// init bl 0x409a90 (main@0x142144). Bypassing fn7f just exposed the next
		// crash at 0x418ea0/0x419164 (bctrl on null r0, because r25=0 — yet another
		// uninitialized manager). The whole 0x409a90 subtree assumes the master
		// manager G[0] exists, which it never does. So skip the ENTIRE 0x409a90 call
		// and fake success (r3=1), to test whether main can still reach an ACTUAL
		// cellGcmInit + WebGPUGSRender::flip without it. If main proceeds far (new
		// MAIN-TRACE addrs past 0x142148, ideally cellGcm activity) -> progress; if
		// main bails immediately -> 0x409a90 is essential and G[0] MUST be built.
		// Guarded on G[0]==0 so it self-disables once the manager exists.
		static const bool k409skip = false; // DISABLED (stackwalk39): let 0x409a90 RUN so GI-RUN can observe registration during main's init
		if (k409skip && cia == 0x409a90 &&
			reinterpret_cast<be_t<u32>*>(mem_ + u64{0x9714f0})->value() == 0) [[unlikely]]
		{
			static thread_local u32 s_409_skip = 0;
			if (s_409_skip++ < 8)
			{
				const auto sp_name = ppu_tname.load();
				const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
				printf("[409a90 SKIP #%u] thread='%s' skip whole init (G[0]=0) -> r3=1 ret to lr=0x%llx\n",
					s_409_skip, nm, static_cast<unsigned long long>(lr));
			}
			gpr[3] = 1;
			cia = static_cast<u32>(lr) & ~3u;
			continue;
		}
		// === 0x4300b0 / 0x43b05c RUNTIME WATCH (stackwalk36) ===
		// 0x4300b0 = find-or-create node in list@arg0 (wlock G's rwlock, 8-byte
		// nodes); 0x43b05c = the OTHER wlock(0x7d) fn (NOT subregion-based) -
		// candidate manager getInstance that reaches G's rwlock via G+0x174 and
		// may store the real >=0x14-byte manager to G[0]. wlock is logged at
		// TRACE (filtered) so syscall-log absence is NOT proof. Watch entries
		// directly: which fns run, with what args, and G[0] before each.
		if (cia == 0x4300b0 || cia == 0x43b05c || cia == 0x424254 || cia == 0x40d188) [[unlikely]]
		{
			static thread_local u32 s_giw = 0;
			if (s_giw++ < 40)
			{
				const u32 g0 = reinterpret_cast<be_t<u32>*>(mem_ + u64{0x9714f0})->value();
				const auto sp_name = ppu_tname.load();
				const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
				printf("[GI-RUN #%u] '%s' fn=0x%x(a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx) G[0]=0x%x lr=0x%llx%s\n",
					s_giw, nm, cia,
					static_cast<unsigned long long>(gpr[3]), static_cast<unsigned long long>(gpr[4]),
					static_cast<unsigned long long>(gpr[5]), static_cast<unsigned long long>(gpr[6]),
					g0, static_cast<unsigned long long>(lr),
					(static_cast<u32>(gpr[3]) == 0x9714f0u) ? "  <-- arg0==&G : sets G[0]!" : "");
			}
		}
		// === CTOR/registration dispatch WATCH (stackwalk37) ===
		// GI-RUN proved the registry-insert fns (0x4300b0/0x43b05c) never run,
		// so the type registries (incl. G[0]'s list) stay empty. Their callers
		// are the static-init region 0x3bfca0/d0/d00 + 0x40d188, dispatched
		// (per stale note) via 0x3bf8a8. Watch whether the dispatcher and the
		// registration fns actually execute, and who calls them (lr).
		if (cia == 0x3bf8a8 || cia == 0x3bfca0 || cia == 0x3bfcd0 || cia == 0x3bfd00 || cia == 0x40d188) [[unlikely]]
		{
			static thread_local u32 s_ctr = 0;
			if (s_ctr++ < 60)
			{
				const auto sp_name = ppu_tname.load();
				const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
				printf("[CTOR-RUN #%u] '%s' fn=0x%x lr=0x%llx r3=0x%llx r2=0x%llx\n",
					s_ctr, nm, cia, static_cast<unsigned long long>(lr),
					static_cast<unsigned long long>(gpr[3]), static_cast<unsigned long long>(gpr[2]));
			}
		}
		// === 0x409a90 SUBTREE error-return TRACE (stackwalk42) ===
		// G[0] is never constructed; the game boots on native rpcs3 so a wasm-
		// specific divergence makes init skip construction. Trace call RETURN
		// values in main's init subtree (cia==0x409a90 .. fn7f@0x7f8e4): a block
		// entry whose preceding instr is bl/bctrl/bclrl is a return point, r3=its
		// return value. Flag error/neg returns (>=0x80000000) - a call failing
		// where the game expects success likely gates the skipped construction.
		{
			static thread_local u32 s_rt_on = 0;
			static thread_local u32 s_rt_n = 0;
			if (cia == 0x409a90) s_rt_on = 1;
			if (cia == 0x7f8e4) s_rt_on = 0;
			if (s_rt_on && s_rt_n < 80)
			{
				const u32 pcia = (cia - 4) & ~3u;
				const u32 pi = reinterpret_cast<be_t<u32>*>(mem_ + u64{pcia})->value();
				const bool isret = ((pi >> 26) == 18 && (pi & 1u) == 1u) || pi == 0x4e800421u || pi == 0x4e800021u;
				const u32 r3 = static_cast<u32>(gpr[3]);
				if (isret && r3 >= 0x80000000u)
				{
					printf("[409RET #%u] ret@0x%x aftercall@0x%x r3=0x%x <-- ERROR/neg\n", s_rt_n, cia, pcia, r3);
					s_rt_n++;
				}
			}
		}
		// === 0x409a90 SUBTREE call-tree TRACE (stackwalk45) ===
		// Map the call tree from main's init (cia==0x409a90) toward fn7f: a block
		// entry whose lr-4 is a bl/bctrl is a CALL entry (target=cia, caller=lr-4).
		// Log target, caller, arg0, and G[0] so I can see the exact function
		// sequence and spot where the manager construction is skipped, or any call
		// after which G[0] flips non-zero (the constructor).
		{
			static thread_local u32 s_ct_on = 0;
			static thread_local u32 s_ct_n = 0;
			if (cia == 0x409a90) s_ct_on = 1;
			if (s_ct_on && s_ct_n < 400)
			{
				const u32 lrm4 = (static_cast<u32>(lr) - 4) & ~3u;
				const u32 li = reinterpret_cast<be_t<u32>*>(mem_ + u64{lrm4})->value();
				const bool isbl = ((li >> 26) == 18 && (li & 1u) == 1u);
				const bool isbctrl = (li == 0x4e800421u);
				bool ok = isbctrl;
				if (isbl) { s32 d = static_cast<s32>(li & 0x03fffffcu); if (d & 0x02000000) d -= 0x04000000; ok = (lrm4 + static_cast<u32>(d) == cia); }
				if (ok)
				{
					const u32 g0 = reinterpret_cast<be_t<u32>*>(mem_ + u64{0x9714f0})->value();
					printf("[409CALL #%u] ->0x%x from 0x%x a0=0x%x G0=0x%x%s\n",
						s_ct_n, cia, lrm4, static_cast<u32>(gpr[3]), g0, isbctrl ? " (bctrl)" : "");
					s_ct_n++;
				}
			}
		}
		// === CTOR8 rldicl interpreter-correctness canary (stackwalk51) ===
		// ctor #8 (0x3bfbe4): `li r4,-1; rldicl r4,r4,0,48` -> r4 MUST be 0xffff (mask
		// keeps low 16 bits; PPC split-field mb=48). Then 0x3bfb58 does real work only
		// `if (r3==1 && r4==0xffff)`. Testable WITHOUT a native trace: the answer is
		// known. If the wasm static interpreter mis-decodes the 64-bit rotate-mask
		// split field, r4 != 0xffff -> guard bails -> work skipped; and such a bug would
		// mis-evaluate MANY branches (plausibly the registration that never runs).
		if (cia == 0x3bfb58) [[unlikely]]
		{
			printf("[CTOR8 guard] @0x3bfb58 r3=0x%llx r4=0x%llx lr=0x%llx %s\n",
				static_cast<unsigned long long>(gpr[3]), static_cast<unsigned long long>(gpr[4]),
				static_cast<unsigned long long>(lr),
				(static_cast<u32>(gpr[4]) == 0xffffu) ? "(rldicl OK)" : "<-- rldicl WRONG: interp bug!");
		}
		if (s_first || !fn->fn) [[unlikely]]
		{
			const auto sp_name = ppu_tname.load();
			const char* nm = (sp_name && !sp_name->empty()) ? sp_name->c_str() : "<unnamed>";
			printf("[exec_task dispatch] thread='%s' cia=0x%x raw=0x%x fn=%p%s\n",
				nm, cia, reinterpret_cast<be_t<u32>*>(mem_ + u64{cia})->value(),
				reinterpret_cast<void*>(fn->fn), fn->fn ? " (first)" : " — NULL would trap unreachable");
			s_first = false;
			if (!fn->fn)
			{
				// Rich one-shot diagnostic: a null dispatch means the guest
				// branched to an address with no decoded instruction (almost
				// always cia==0 → it called a function pointer that read back
				// as 0). On wasm32 this is a correctness bug, not a missing
				// HLE export (the same games boot on native rpcs3). Dump the
				// register file + the raw call-site instructions so we can
				// decode WHICH pointer load produced the 0. Capped so other
				// threads hitting null later don't flood the console.
				static thread_local u32 s_nullcnt = 0;
				if (s_nullcnt++ < 6)
				{
					const auto rd = [mem_](u32 a) -> u32 {
						return reinterpret_cast<be_t<u32>*>(mem_ + u64{a})->value();
					};
					printf("[null-fn DIAG #%u] thread='%s' cia=0x%x lr=0x%llx ctr=0x%llx r2(TOC)=0x%llx\n",
						s_nullcnt, nm, cia, lr, ctr, gpr[2]);
					// Full GPR file. r25-r31 are callee-saved and almost always
					// hold the C++ `this` / object / function-descriptor pointer
					// that read back null — dumping them tells us WHICH object.
					for (u32 r = 0; r < 32; r += 4)
					{
						printf("  r%u=%llx r%u=%llx r%u=%llx r%u=%llx\n",
							r, gpr[r], r + 1, gpr[r + 1], r + 2, gpr[r + 2], r + 3, gpr[r + 3]);
					}
					// Call site: dump a WIDE window backward (0x100 bytes = 64
					// instrs) so we capture the function prologue and the actual
					// load that set the null object register (lwz/ld r27 =
					// 0x836.. / 0xEB6.. , addi r27 = 0x3B6.., mr r27 = 0x7..B378).
					// The bctrl/bclrl itself is at base-4.
					const u32 base = static_cast<u32>(lr) & ~3u;
					for (u32 k = 0x100; k >= 4; k -= 0x10)
					{
						printf("  cs @0x%x: %08x %08x %08x %08x\n", base - k,
							rd(base - k), rd(base - k + 4), rd(base - k + 8), rd(base - k + 0xc));
					}
					// Deref the callee-saved object-pointer candidates: the 8
					// bytes at each (an OPD reads as [entry,toc]; an object reads
					// as [vtable_ptr, ...]). A null/zero result pinpoints the bad one.
					for (u32 r = 24; r <= 31; r++)
					{
						const u32 p = static_cast<u32>(gpr[r]);
						printf("  *r%u(0x%x)=[%08x %08x]\n", r, p, rd(p), rd(p + 4));
					}
					// Recover the game TOC saved on the stack: ELFv1 frames do
					// `std r2,0x28(r1)` before an indirect call, so *(sp+0x28) is
					// the caller's TOC even though r2 itself was clobbered to the
					// (null) callee TOC. For the known 0x7f9cc site, walk the
					// global→singleton→field chain that produced the null:
					//   r6=*(TOC-0x7780); r5=*r6; r28=*(r5+0x10)  ← the root null.
					// This distinguishes a DATA-LOAD bug (global/r6 itself null)
					// from an INIT bug (global valid, but singleton field +0x10 null).
					const u32 sp_ = static_cast<u32>(gpr[1]);
					const u32 saved_toc = rd(sp_ + 0x28);
					printf("  saved_toc(@sp+0x28)=0x%x\n", saved_toc);
					// Back-chain stack walk (PPC64 ELFv1: *(sp)=caller's sp;
					// *(caller_sp+0x10)=the LR saved by the callee = return addr
					// into its caller). Recovers the CALL PATH that reached this
					// null indirect call. Decisive for the "C++ global ctors never
					// run" theory: if the path traces back through the CRT init
					// routines (0x492270/0x49bd24/0x497dd0/0x49c1a0/0x49ce40 — the
					// bl-sequence in _start at 0x102ac+), 0x7f8e4 is a global ctor
					// (or the ctor-iterator) and the bug is in ctor dispatch; if it
					// traces back through main(), the singleton init was simply
					// never invoked before first use.
					// PPC64 ELFv1 frames are 64-bit doublewords: the 32-bit vm
					// address sits in the LOW half, so back-chain = *(sp+4) and the
					// callee-saved LR = *(caller_sp+0x14) (the +0x10 doubleword's
					// low word). Reading +0/+0x10 yields the zero high word.
					{
						u32 wsp = sp_;
						for (u32 i = 0; i < 24; i++)
						{
							const u32 back = rd(wsp + 4);
							if (back == 0 || back <= wsp) { printf("  [stk] #%u sp=0x%x <end back=0x%x>\n", i, wsp, back); break; }
							const u32 slr = rd(back + 0x14);
							// Also dump the call site (the 2 instrs preceding the
							// return addr): identifies a `bl`/`bctrl`/`blrl` and lets
							// us name each caller function and decode the CRT path.
							printf("  [stk] #%u sp=0x%x caller_sp=0x%x ret=0x%x callsite[%08x %08x]\n",
								i, wsp, back, slr, rd((slr - 8) & ~3u), rd((slr - 4) & ~3u));
							wsp = back;
						}
						// One-shot: dump the CRT main (the function _start calls at
						// 0x1024c -> 0x10354). Reading its body reveals whether it
						// runs an .init_array/global-ctor loop before calling main,
						// and where each chain return addr (0x104d8 etc.) lands.
						static atomic_t<u32> s_crt_dump{0};
						if (s_crt_dump.exchange(1) == 0)
						{
							for (u32 a = 0x10354; a <= 0x10354 + 0x190; a += 0x10)
								printf("  crt @0x%x: %08x %08x %08x %08x\n", a, rd(a), rd(a + 4), rd(a + 8), rd(a + 0xc));
							// CTORS CONFIRMED RUNNING (cap_stackwalk12): all 26 dispatch
							// via 0x3bf8a8 to valid code. Crash is in main (0x141d84):
							// main@0x142144 -> 0x40ea90 -> 0x409a90 release -> null vtable
							// call. Dump main's body up to the crash call (to read its
							// init `bl` sequence + which call leads to 0x142144) and the
							// crash function 0x40ea90, so we can find the FIRST init step
							// that fails (returns 0/error) and leaves a .bss object zero.
							for (u32 a = 0x141d84; a <= 0x142150; a += 0x10)
								printf("  main @0x%x: %08x %08x %08x %08x\n", a, rd(a), rd(a + 4), rd(a + 8), rd(a + 0xc));
							for (u32 a = 0x40ea90; a <= 0x40ea90 + 0xb0; a += 0x10)
								printf("  f40ea90 @0x%x: %08x %08x %08x %08x\n", a, rd(a), rd(a + 4), rd(a + 8), rd(a + 0xc));
							// SIBLING REGISTRY: 0x40ea90 compares r4 against *(TOC+0x5a14).
							// If this sibling is non-null while G[0] is null, the failure
							// is G-specific (one manager's init was skipped), not systemic.
							{
								const u32 sib = rd(0x5ace5c); // = *(eboot_toc + 0x5a14)
								printf("  [REG] sibling *(TOC+0x5a14)=*0x5ace5c=0x%x deref=0x%x [%08x %08x %08x %08x]\n",
									sib, rd(sib), rd(sib), rd(sib + 4), rd(sib + 8), rd(sib + 0xc));
							}
							// LOCATE G[0]'s INITIALIZER: G is loaded via `lwz rX,-0x7780(r2)`
							// (== (word & 0xFC1FFFFF)==0x80028880). Its first-field writer
							// does `stw rZ,0(rX)` within a few instrs. Scan the loaded EBOOT
							// for that pair so we can name the function that SHOULD set G[0]
							// and trace why it's never executed.
							{
								// ROBUST G-SETTER FINDER. G is the game global pointer at
								// *(TOC-0x7780)==0x9714f0; it should be written by
								//   lwz rA,-0x7780(r2) ; ...(maybe calls)... ; stw/std rZ,0(rA)
								// The earlier tight lwz+stw(<=20,same-reg) scan found 0 writers.
								// Widen decisively: scan EVERY store-to-0(rA) in the EBOOT text
								// (stw op36 D==0, AND std op62 DS==0 — pointers may be 8-byte),
								// then back-search up to 64 instrs for the G-load into the SAME
								// base rA. Callee-saved bases survive `bl`, so the store can be
								// far from the load. This finds the setter even when it spans
								// allocator/ctor calls.
								u32 setters = 0;
								for (u32 a = 0x10000; a < 0x588ce8; a += 4)
								{
									const u32 w = rd(a);
									const u32 op = w >> 26;
									u32 rZ = 0, rA = 0; bool store0 = false; const char* knd = "stw";
									if (op == 36 && (w & 0xffffu) == 0) { store0 = true; rZ = (w >> 21) & 0x1f; rA = (w >> 16) & 0x1f; knd = "stw"; }
									else if (op == 62 && (w & 0x3u) == 0 && (w & 0xfffcu) == 0) { store0 = true; rZ = (w >> 21) & 0x1f; rA = (w >> 16) & 0x1f; knd = "std"; }
									if (!store0 || rA == 0) continue;
									for (u32 j = 1; j <= 64; j++)
									{
										const u32 wb = rd(a - j * 4);
										if ((wb & 0xFC1FFFFFu) == 0x80028880u && ((wb >> 21) & 0x1f) == rA)
										{
											if (setters < 40)
												printf("  [G-SET] store@0x%x %s r%u,0(r%u) <- G-load@0x%x gap=%u\n",
													a, knd, rZ, rA, a - j * 4, j);
											setters++;
											break;
										}
									}
								}
								// Enumerate G-load sites and flag LAZY-INIT candidates: a
								// getInstance does `lwz rX,G; lwz rY,0(rX); cmp[l]wi rY,0; b<cc>`
								// before allocating. Finding the null-check site points to the
								// function that SHOULD construct G[0] on first use.
								u32 gloads = 0, checks = 0;
								for (u32 a = 0x10000; a < 0x588ce8 && gloads < 40; a += 4)
								{
									const u32 w = rd(a);
									if ((w & 0xFC1FFFFFu) != 0x80028880u) continue;
									const u32 rX = (w >> 21) & 0x1f;
									gloads++;
									bool lazy = false; u32 chk_at = 0;
									for (u32 j = 1; j <= 8; j++)
									{
										const u32 w2 = rd(a + j * 4);
										if (((w2 >> 26) == 11 || (w2 >> 26) == 10) && (w2 & 0xffffu) == 0) { lazy = true; chk_at = a + j * 4; break; }
									}
									if (lazy) { checks++; printf("  [G-CHK] @0x%x lwz r%u,G then cmp-zero@0x%x (lazy-init candidate)\n", a, rX, chk_at); }
								}
								printf("  [G-SCAN] setters(stw/std@0,<=64back)=%u gloads=%u nullcheck-sites=%u\n",
									setters, gloads, checks);
								// === G[0] CONSTRUCTOR HUNT ===
								// stackwalk25 settled it: the only -0x7780(r2) writer (0x80170)
								// lives INSIDE the big crash fn 0x7f8e4 (stdu r1,-688 prologue,
								// no intervening prologue down to 0x80178). That fn READS G[0]
								// at 0x7f940 (null-deref @0x7f944) AND reads OLD G[0]->0x10 at
								// 0x80114 before re-storing at 0x80170 — so it ASSUMES G[0] is
								// already valid. It is NOT the constructor. The real ctor must
								// form &G=0x9714f0 with the absolute lis/addi pair (typical
								// static-init for a file-scope global) and store the allocated
								// manager there. Hunt it: lis rA,0x97 ; addi/ori rA,rA,0x14f0 ;
								// then stw/std rZ,0(rA).
								{
									u32 ctors = 0;
									for (u32 a = 0x10000; a < 0x588ce8 && ctors < 24; a += 4)
									{
										const u32 w = rd(a);
										if ((w >> 26) != 15) continue;          // addis
										if (((w >> 16) & 0x1f) != 0) continue;  // lis: RA(src)=0
										if ((w & 0xffffu) != 0x0097u) continue; // hi(0x9714f0)
										const u32 rA = (w >> 21) & 0x1f;
										u32 lo_at = 0;
										for (u32 j = 1; j <= 8; j++)
										{
											const u32 w2 = rd(a + j * 4);
											const u32 op2 = w2 >> 26;
											const bool lo = (op2 == 14 || op2 == 24)
												&& ((w2 >> 21) & 0x1f) == rA
												&& ((w2 >> 16) & 0x1f) == rA
												&& (w2 & 0xffffu) == 0x14f0u;
											if (lo) { lo_at = a + j * 4; break; }
										}
										if (!lo_at) continue;
										u32 store_at = 0, rZ = 0; const char* knd = "none";
										for (u32 j = 1; j <= 48; j++)
										{
											const u32 w3 = rd(lo_at + j * 4);
											const u32 op3 = w3 >> 26;
											if (op3 == 36 && ((w3 >> 16) & 0x1f) == rA && (w3 & 0xffffu) == 0)
												{ store_at = lo_at + j * 4; rZ = (w3 >> 21) & 0x1f; knd = "stw"; break; }
											if (op3 == 62 && ((w3 >> 16) & 0x1f) == rA && (w3 & 0x3u) == 0 && (w3 & 0xfffcu) == 0)
												{ store_at = lo_at + j * 4; rZ = (w3 >> 21) & 0x1f; knd = "std"; break; }
										}
										printf("  [G-CTOR] lis@0x%x lo@0x%x rA=r%u store=%s r%u,0(rA)@0x%x\n",
											a, lo_at, rA, knd, rZ, store_at);
										ctors++;
									}
									printf("  [G-CTOR] %u absolute &G(0x9714f0) formation site(s)\n", ctors);

									// Confirm 0x80170 is inside crash-fn 0x7f8e4: next prologue
									// (stdu/stwu r1,-N(r1)) after it must be > 0x80170.
									u32 nextfn = 0;
									for (u32 a = 0x7f8e8; a < 0x81000 && !nextfn; a += 4)
									{
										const u32 w = rd(a);
										const u32 op = w >> 26;
										const bool stdu1 = (op == 62) && (((w >> 21) & 0x1f) == 1) && (((w >> 16) & 0x1f) == 1) && ((w & 3u) == 1);
										const bool stwu1 = (op == 37) && (((w >> 21) & 0x1f) == 1) && (((w >> 16) & 0x1f) == 1);
										if (stdu1 || stwu1) nextfn = a;
									}
									printf("  [G-CTOR] crash-fn 0x7f8e4 nextProlog@0x%x setter0x80170-inside=%s\n",
										nextfn, (nextfn && 0x80170u < nextfn) ? "yes" : "no");
								}

								// === ABSOLUTE-DISPLACEMENT G[0] WRITER SCAN (stackwalk31) ===
								// stackwalk30 DISPROVED the body#1 theory: body#1 (0x42fa28) ONLY
								// calls helper 0x42f8e8 which creates G's rwlock at G+0x174
								// (li r4,0=null attr; li r11,0x78; sc) and returns — no manager
								// alloc, no G[0] store. So the rwlock guards LAZY construction
								// (double-checked-locking singleton) and the real getInstance was
								// MISSED by prior scans. They only checked the TOC-load+store@0 and
								// the addi-formed absolute. The UNCHECKED form is the displacement-
								// folded absolute store: `lis rA,0x97 [; addi rA,rA,k] ; stw/std
								// rZ,0x14f0(rA)` (0x970000+0x14f0 = 0x9714f0 = &G[0]) — the usual
								// compiler output for a file-scope pointer write. Scan for it.
								{
									u32 hits = 0;
									for (u32 a = 0x10000; a < 0x588ce8 && hits < 40; a += 4)
									{
										const u32 w = rd(a);
										if ((w >> 26) != 15) continue;          // addis
										if (((w >> 16) & 0x1f) != 0) continue;  // lis: RA(src)=0
										if ((w & 0xffffu) != 0x0097u) continue; // hi = 0x97
										const u32 rA = (w >> 21) & 0x1f;
										s32 adj = 0;
										for (u32 j = 1; j <= 48; j++)
										{
											const u32 w2 = rd(a + j * 4);
											const u32 op2 = w2 >> 26;
											if (op2 == 14 && ((w2 >> 21) & 0x1f) == rA && ((w2 >> 16) & 0x1f) == rA)
											{ adj += static_cast<s16>(w2 & 0xffffu); continue; } // addi rA,rA,k
											if (op2 == 36 && ((w2 >> 16) & 0x1f) == rA)            // stw rZ,d(rA)
											{
												const s32 d = static_cast<s16>(w2 & 0xffffu);
												if (d + adj == 0x14f0)
												{ printf("  [G-ABS] lis@0x%x rA=r%u stw r%u,%d(rA)@0x%x adj=%d -> &G[0]\n",
													a, rA, (w2 >> 21) & 0x1f, d, a + j * 4, adj); hits++; break; }
											}
											if (op2 == 62 && ((w2 >> 16) & 0x1f) == rA && (w2 & 3u) == 0) // std rZ,d(rA)
											{
												const s32 d = static_cast<s16>(w2 & 0xfffcu);
												if (d + adj == 0x14f0)
												{ printf("  [G-ABS] lis@0x%x rA=r%u std r%u,%d(rA)@0x%x adj=%d -> &G[0]\n",
													a, rA, (w2 >> 21) & 0x1f, d, a + j * 4, adj); hits++; break; }
											}
											if (op2 == 15 && ((w2 >> 21) & 0x1f) == rA) break; // base reloaded
										}
									}
									printf("  [G-ABS] %u absolute lis-0x97+store@0x14f0 site(s)\n", hits);
								}
								// === getInstance LOCATOR via rwlock LOCK syscall (stackwalk34) ===
								// Store-pattern scans (G-SET/G-CTOR/G-ABS) all failed: the ctor gets
								// &G as an ARG (store `stw rZ,0(rArg)` in a callee, provenance in the
								// caller). The DCL getInstance is immune - it MUST issue a rwlock LOCK
								// syscall on G's rwlock. lv2: 0x78 create,0x79 destroy,0x7a rlock,
								// 0x7c runlock,0x7d wlock,0x7f wunlock. Scan EBOOT for
								// `li r11,N(0x78..0x7f) ; sc` + each site's fn entry (back-scan to
								// stwu/stdu r1 prologue). The 0x7d(wlock) site's fn == getInstance or
								// its lock-helper. Runtime shows NO wlock executes => getInstance is
								// compiled but never invoked (the real question).
								{
									u32 lk = 0;
									for (u32 a = 0x10000; a < 0x588ce8 && lk < 40; a += 4)
									{
										const u32 w = rd(a);
										if ((w >> 26) != 14) continue;            // addi
										if (((w >> 21) & 0x1f) != 11) continue;   // rD=r11
										if (((w >> 16) & 0x1f) != 0) continue;    // rA=0 => li r11,N
										const u32 num = w & 0xffffu;
										if (num < 0x78 || num > 0x7f) continue;   // rwlock family
										u32 sc_at = 0;
										for (u32 j = 1; j <= 8 && !sc_at; j++)
											if (rd(a + j * 4) == 0x44000002u) sc_at = a + j * 4;
										if (!sc_at) continue;
										u32 fent = 0;
										for (u32 b = a; b > 0x10000 && a - b < 0x800 && !fent; b -= 4)
										{
											const u32 wb = rd(b); const u32 ob = wb >> 26;
											const bool p62 = (ob == 62) && (((wb >> 21) & 0x1f) == 1) && (((wb >> 16) & 0x1f) == 1) && ((wb & 3u) == 1);
											const bool p37 = (ob == 37) && (((wb >> 21) & 0x1f) == 1) && (((wb >> 16) & 0x1f) == 1);
											if (p62 || p37) fent = b;
										}
										printf("  [RWL-LK] li r11,0x%x @0x%x sc@0x%x fnEntry=0x%x\n", num, a, sc_at, fent);
										lk++;
									}
									printf("  [RWL-LK] %u rwlock lock/unlock syscall sites\n", lk);
									// getInstance reaches G's rwlock id (0x971664) via sub-region ptr
									// 0x9715b0 loaded from TOC+0x5bac. `lwz rX,0x5bac(r2)`==0x80025bac
									// (masked over rD). The fn that loads subrgn AND is a [RWL-LK]
									// fnEntry is getInstance.
									u32 sr = 0;
									for (u32 a = 0x10000; a < 0x588ce8 && sr < 30; a += 4)
									{
										const u32 w = rd(a);
										if ((w & 0xFC1FFFFFu) != 0x80025bacu) continue;
										u32 fent = 0;
										for (u32 b = a; b > 0x10000 && a - b < 0x800 && !fent; b -= 4)
										{
											const u32 wb = rd(b); const u32 ob = wb >> 26;
											const bool p62 = (ob == 62) && (((wb >> 21) & 0x1f) == 1) && (((wb >> 16) & 0x1f) == 1) && ((wb & 3u) == 1);
											const bool p37 = (ob == 37) && (((wb >> 21) & 0x1f) == 1) && (((wb >> 16) & 0x1f) == 1);
											if (p62 || p37) fent = b;
										}
										printf("  [SUBRGN] lwz r%u,0x5bac(r2)@0x%x fnEntry=0x%x\n", (w >> 21) & 0x1f, a, fent);
										sr++;
									}
									printf("  [SUBRGN] %u subregion-ptr load sites\n", sr);
								}
								// === getInstance(0x4300b0) BODY + CALLER discovery (stackwalk35) ===
								// 0x4300b0 loads G's subregion (0x5bac(r2)) AND wlocks(0x7d)/wunlocks
								// (0x7f) G's rwlock => it IS the DCL constructor of G[0]. Runtime shows
								// it NEVER runs (no wlock syscall). To make the GAME's real ctor build
								// G[0] honestly we must either fix why its natural caller is skipped or
								// force-call it. Both need: (a) its prologue/args, (b) who calls it.
								{
									// (a) disasm getInstance body (prologue + arg usage + what it builds)
									for (u32 p = 0x4300b0; p < 0x4300b0 + 0x1c0; p += 0x10)
										printf("  [GI-DIS] @0x%x: %08x %08x %08x %08x\n", p, rd(p), rd(p + 4), rd(p + 8), rd(p + 0xc));
									// (b) direct `bl 0x4300b0` callers (op18,LK=1,AA=0; tgt=a+sext(LI))
									u32 callers = 0;
									for (u32 a = 0x10000; a < 0x588ce8 && callers < 30; a += 4)
									{
										const u32 w = rd(a);
										if ((w >> 26) != 18) continue;
										if ((w & 1u) != 1u) continue;   // LK=1 (bl)
										if ((w & 2u) != 0u) continue;   // AA=0 (relative)
										s32 li = static_cast<s32>(w & 0x03fffffcu);
										if (li & 0x02000000) li -= 0x04000000;
										if (a + static_cast<u32>(li) != 0x4300b0u) continue;
										u32 fent = 0;
										for (u32 b = a; b > 0x10000 && a - b < 0x1000 && !fent; b -= 4)
										{
											const u32 wb = rd(b); const u32 ob = wb >> 26;
											const bool p62 = (ob == 62) && (((wb >> 21) & 0x1f) == 1) && (((wb >> 16) & 0x1f) == 1) && ((wb & 3u) == 1);
											const bool p37 = (ob == 37) && (((wb >> 21) & 0x1f) == 1) && (((wb >> 16) & 0x1f) == 1);
											if (p62 || p37) fent = b;
										}
										printf("  [GI-CALL] bl 0x4300b0 @0x%x callerFn=0x%x [ctx %08x %08x %08x]\n", a, fent, rd(a - 4), rd(a), rd(a + 4));
										callers++;
									}
									printf("  [GI-CALL] %u direct bl callers of getInstance\n", callers);
									// (c) data words == 0x4300b0 (OPD entry / fn-ptr table -> indirect call)
									u32 refs = 0;
									for (u32 a = 0x10000; a < 0x600000 && refs < 30; a += 4)
									{
										if (rd(a) != 0x4300b0u) continue;
										printf("  [GI-PTR] word 0x4300b0 @0x%x next=0x%x\n", a, rd(a + 4));
										refs++;
									}
									printf("  [GI-PTR] %u data refs to 0x4300b0\n", refs);
								}
								// === disasm 0x43b05c = the OTHER wlock(0x7d) fn (stackwalk36) ===
								// Not subregion(0x5bac)-based, so it reaches its rwlock another way -
								// candidate real manager getInstance (wlock G+0x174, alloc manager,
								// store to G[0]). Disasm prologue/args + its TOC slot & store offsets.
								{
									for (u32 p = 0x43b05c; p < 0x43b05c + 0x180; p += 0x10)
										printf("  [GI2-DIS] @0x%x: %08x %08x %08x %08x\n", p, rd(p), rd(p + 4), rd(p + 8), rd(p + 0xc));
								}
								// === disasm ctor/registration region + ctor-table scan (stackwalk37) ===
								// Resolve the contradiction: is 0x3bf8a8 reached & does it reach the
								// bl 0x4300b0 sites (0x3bfcb4/ce4/d14)? Disasm entry+region to find the
								// gate. Then find 0x3bf8a8 / 0x3bfca0 / 0x40d188 as data (ctor-table or
								// OPD entries) to learn how static init was SUPPOSED to invoke them.
								{
									for (u32 p = 0x3bf880; p < 0x3bfd30; p += 0x10)
										printf("  [CTOR-DIS] @0x%x: %08x %08x %08x %08x\n", p, rd(p), rd(p + 4), rd(p + 8), rd(p + 0xc));
									u32 cp = 0;
									for (u32 a = 0x10000; a < 0x600000 && cp < 40; a += 4)
									{
										const u32 w = rd(a);
										if (w == 0x3bf8a8u || w == 0x3bfca0u || w == 0x3bfcd0u || w == 0x3bfd00u || w == 0x40d188u)
										{
											printf("  [CTOR-PTR] word 0x%x @0x%x [prev=0x%x next=0x%x]\n", w, a, rd(a - 4), rd(a + 4));
											cp++;
										}
									}
									printf("  [CTOR-PTR] %u data refs to ctor/registration fns\n", cp);
								}
								// === CTOR TABLE dump (stackwalk38) ===
								// 0x3bf8a8 iterates the global-ctor table: head=*(TOC+0x515c), start at
								// head-4, walk DOWN by 4 calling each entry's OPD until entry==-1. Dump
								// the raw table + resolve each OPD->fn. If the first entry is -1 (or the
								// table is short/empty) the loader never populated it => no ctors run =>
								// no type-registration => G[0]=0. Also dump the registration-thunk vtable
								// at 0x598948 (0x3bfca0/d0/d00 OPDs) for layout.
								{
									const u32 head = rd(eboot_toc + 0x515c);
									printf("  [CTORTAB] head=*(TOC+0x515c)=0x%x iterStart=0x%x\n", head, head - 4);
									for (u32 p = 0x590000; p < 0x590078; p += 0x10)
										printf("  [CTORTAB] raw @0x%x: %08x %08x %08x %08x\n", p, rd(p), rd(p + 4), rd(p + 8), rd(p + 0xc));
									u32 cnt = 0;
									for (u32 e = head - 4; e >= 0x590000u && cnt < 40; e -= 4)
									{
										const u32 ent = rd(e);
										if (ent == 0xffffffffu) { printf("  [CTORTAB] @0x%x = -1 END after %u ctors\n", e, cnt); break; }
										printf("  [CTORTAB] #%u @0x%x -> OPD 0x%x -> fn 0x%x\n", cnt, e, ent, rd(ent));
										cnt++;
									}
									printf("  [CTORTAB] walked %u entries\n", cnt);
									for (u32 p = 0x598940; p < 0x598980; p += 0x10)
										printf("  [REGVT] @0x%x: %08x %08x %08x %08x\n", p, rd(p), rd(p + 4), rd(p + 8), rd(p + 0xc));
								}
								// === REGISTRATION DRIVER hunt (stackwalk40) ===
								// Registration (0x4300b0) never runs anywhere. Find the driver by tracing
								// UP: (a) bl callers of 0x40d188 (a direct 0x4300b0 caller not in REGVT);
								// (b) data refs to REGVT base 0x598940 / 0x40d188 / its OPD 0x59b888;
								// (c) code forming 0x598940 (lis r,0x59 ; addi r,r,0x8940). The fn that
								// references REGVT and bctrl-iterates it IS the registration driver.
								{
									u32 c1 = 0;
									for (u32 a = 0x10000; a < 0x588ce8 && c1 < 24; a += 4)
									{
										const u32 w = rd(a);
										if ((w >> 26) != 18 || (w & 1u) != 1u || (w & 2u) != 0u) continue;
										s32 li = static_cast<s32>(w & 0x03fffffcu); if (li & 0x02000000) li -= 0x04000000;
										const u32 tgt = a + static_cast<u32>(li);
										if (tgt != 0x40d188u && tgt != 0x3bfc70u && tgt != 0x3bfca0u) continue;
										printf("  [REGDRV] bl 0x%x @0x%x\n", tgt, a);
										c1++;
									}
									printf("  [REGDRV] %u bl callers of 0x40d188/thunks\n", c1);
									u32 c2 = 0;
									for (u32 a = 0x10000; a < 0x600000 && c2 < 30; a += 4)
									{
										const u32 w = rd(a);
										if (w == 0x598940u || w == 0x59b888u || w == 0x40d188u)
											{ printf("  [REGDRV] word 0x%x @0x%x [prev=0x%x next=0x%x]\n", w, a, rd(a - 4), rd(a + 4)); c2++; }
									}
									printf("  [REGDRV] %u data refs (REGVT/0x40d188)\n", c2);
									u32 c3 = 0;
									for (u32 a = 0x10000; a < 0x588ce8 && c3 < 20; a += 4)
									{
										const u32 w = rd(a);
										if ((w >> 26) != 15 || ((w >> 16) & 0x1f) != 0 || (w & 0xffffu) != 0x0059u) continue;
										const u32 rA = (w >> 21) & 0x1f;
										for (u32 j = 1; j <= 8; j++)
										{
											const u32 w2 = rd(a + j * 4); const u32 op2 = w2 >> 26;
											if ((op2 == 14 || op2 == 24) && ((w2 >> 21) & 0x1f) == rA && ((w2 >> 16) & 0x1f) == rA && (w2 & 0xffffu) == 0x8940u)
												{ printf("  [REGDRV] lis 0x59@0x%x +0x8940@0x%x r%u -> REGVT base\n", a, a + j * 4, rA); c3++; break; }
										}
									}
									printf("  [REGDRV] %u lis/addi forming 0x598940\n", c3);
								}
								// === re-disasm fn7f (0x7f8e4) the actual crash consumer (stackwalk41) ===
								// Re-ground: my whole 'G[0]=registry head' model rests on a stale reading
								// of fn7f. Disasm 0x7f8e4..+0x150 to verify EXACTLY what it loads from G
								// (=*(TOC-0x7780)=0x9714f0), which field it derefs, and the null bclrl at
								// 0x7f9c8 - so I know precisely what state G[0] must be in.
								{
									for (u32 p = 0x7f8e4; p < 0x7f8e4 + 0x150; p += 0x10)
										printf("  [FN7F-DIS] @0x%x: %08x %08x %08x %08x\n", p, rd(p), rd(p + 4), rd(p + 8), rd(p + 0xc));
									// current G geometry at crash time
									const u32 Gp = rd(eboot_toc - 0x7780);
									printf("  [FN7F-DIS] &G slot=*(TOC-0x7780)=0x%x  G[0]=*0x9714f0=0x%x\n", Gp, rd(0x9714f0));
								}
								// === refined G[0]-setter scan (stackwalk43) ===
								// The manager ctor loads &G into a CALLEE-SAVED reg (r25-r31, survives the
								// new()+ctor bl chain) then stores the manager ptr: stw/std rZ,0(rA) with
								// rA in r25..r31 and `lwz rA,-0x7780(r2)` within 300 instrs before. Prior
								// G-SET (64-instr, any rA) found only volatile-r3 false positives.
								{
									u32 hits = 0;
									for (u32 x = 0x10000; x < 0x588ce8 && hits < 40; x += 4)
									{
										const u32 w = rd(x); const u32 op = w >> 26;
										u32 rZ = 0, rA = 0; bool st = false; const char* kn = "stw";
										if (op == 36 && (w & 0xffffu) == 0) { st = true; rZ = (w >> 21) & 0x1f; rA = (w >> 16) & 0x1f; kn = "stw"; }
										else if (op == 62 && (w & 0x3u) == 0 && (w & 0xfffcu) == 0) { st = true; rZ = (w >> 21) & 0x1f; rA = (w >> 16) & 0x1f; kn = "std"; }
										if (!st || rA < 25 || rA > 31) continue;
										for (u32 j = 1; j <= 300; j++)
										{
											const u32 wb = rd(x - j * 4);
											if ((wb & 0xFC1FFFFFu) == 0x80028880u && ((wb >> 21) & 0x1f) == rA)
											{
												printf("  [GSET2] %s r%u,0(r%u) @0x%x <- &G-load@0x%x gap=%u\n", kn, rZ, rA, x, x - j * 4, j);
												hits++; break;
											}
											const u32 ob = wb >> 26;
											if ((ob == 32 || ob == 14 || ob == 15) && ((wb >> 21) & 0x1f) == rA) break; // rA redefined (not the G-load) -> chain broken
										}
									}
									printf("  [GSET2] %u callee-saved G[0] setters\n", hits);
								}
								// === REGVT-driver hunt: references to the registrar objects (stackwalk46) ===
								// REGVT base 0x598940 is stored as data in static objects at 0x59105c and
								// 0x591e2c (no code forms it). The driver loads REGVT from there and bctrl-
								// iterates it. Find who reaches those objects: data ptrs to 0x591058/0x591e28
								// region (the array/list of registrars) + code forming 0x5910xx/0x591exx.
								{
									u32 c1 = 0;
									for (u32 x = 0x10000; x < 0x600000 && c1 < 40; x += 4)
									{
										const u32 w = rd(x);
										if ((w >= 0x591040u && w < 0x591070u) || (w >= 0x591e10u && w < 0x591e40u))
											{ printf("  [REGREF] ptr 0x%x @0x%x [prev=0x%x next=0x%x]\n", w, x, rd(x - 4), rd(x + 4)); c1++; }
									}
									printf("  [REGREF] %u data ptrs to registrar region\n", c1);
									u32 c2 = 0;
									for (u32 x = 0x10000; x < 0x588ce8 && c2 < 24; x += 4)
									{
										const u32 w = rd(x);
										if ((w >> 26) != 15 || ((w >> 16) & 0x1f) != 0 || (w & 0xffffu) != 0x0059u) continue;
										const u32 rA = (w >> 21) & 0x1f;
										for (u32 j = 1; j <= 8; j++)
										{
											const u32 w2 = rd(x + j * 4); const u32 op2 = w2 >> 26;
											if ((op2 == 14 || op2 == 24) && ((w2 >> 21) & 0x1f) == rA && ((w2 >> 16) & 0x1f) == rA)
											{
												const u32 lo = w2 & 0xffffu;
												if ((lo >= 0x1040u && lo < 0x1070u) || (lo >= 0x1e10u && lo < 0x1e40u))
													printf("  [REGREF] lis0x59@0x%x +0x%x@0x%x r%u -> registrar\n", x, lo, x + j * 4, rA);
												break;
											}
										}
									}
									printf("  [REGREF] done lis/addi scan\n");
								}
								// === REGVT driver = loader of registrar TOC slots (stackwalk47) ===
								// Registrar 0x591040 is in TOC+0x51b0; 0x591e10 in TOC+0x55f4. The driver
								// does `lwz rX,0x51b0(r2)` (or 0x55f4) then reads REGVT at +0x1c and bctrl-
								// iterates. lwz rX,0x51b0(r2) == (w & 0xFC1FFFFF)==0x800251b0. Find every
								// such loader + its fn entry (back-scan to stdu/stwu r1) = the driver fns.
								{
									u32 c = 0;
									for (u32 x = 0x10000; x < 0x588ce8 && c < 30; x += 4)
									{
										const u32 w = rd(x);
										const u32 m = w & 0xFC1FFFFFu;
										if (m != 0x800251b0u && m != 0x800255f4u) continue;
										u32 fent = 0;
										for (u32 b = x; b > 0x10000 && x - b < 0x1000 && !fent; b -= 4)
										{
											const u32 wb = rd(b); const u32 ob = wb >> 26;
											const bool p62 = (ob == 62) && (((wb >> 21) & 0x1f) == 1) && (((wb >> 16) & 0x1f) == 1) && ((wb & 3u) == 1);
											const bool p37 = (ob == 37) && (((wb >> 21) & 0x1f) == 1) && (((wb >> 16) & 0x1f) == 1);
											if (p62 || p37) fent = b;
										}
										printf("  [REGDRV2] lwz r%u,0x%x(r2)@0x%x fnEntry=0x%x\n", (w >> 21) & 0x1f, w & 0xffffu, x, fent);
										c++;
									}
									printf("  [REGDRV2] %u TOC-registrar loaders (driver candidates)\n", c);
								}
							}
						}
					}
					// The faulting sites at 0x7f9cc / 0x7fa84 both load a single
					// game global G via `lwz rX,-0x7780(r2)` (r2 = EBOOT TOC). The
					// on-stack saved_toc is unreliable here (mid-block indirect
					// calls zero r2 before std r2 saves it), so compute the slot
					// from the KNOWN EBOOT TOC instead. This answers the decisive
					// question: is the global pointer slot itself null (G never
					// got its pointer → init/ctor never ran), or is G valid but
					// its manager field G[0x10] null (sub-object init skipped)?
					if ((base == 0x7f9cc || base == 0x7fa84) && eboot_toc)
					{
						const u32 slot = eboot_toc - 0x7780; // expected 0x59fcc8
						const u32 G    = rd(slot);            // the game global ptr
						const u32 mgr  = rd(G + 0x10);        // G[0x10] = manager
						printf("  CHAIN etoc=0x%x slot=0x%x G=*slot=0x%x G[0]=0x%x G[0x10]=mgr=0x%x\n",
							eboot_toc, slot, G, rd(G), mgr);
						printf("  G[0x00..0x20]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
							rd(G), rd(G + 4), rd(G + 8), rd(G + 0xc),
							rd(G + 0x10), rd(G + 0x14), rd(G + 0x18), rd(G + 0x1c));
						if (mgr)
						{
							printf("  mgr[0x00..0x20]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
								rd(mgr), rd(mgr + 4), rd(mgr + 8), rd(mgr + 0xc),
								rd(mgr + 0x10), rd(mgr + 0x14), rd(mgr + 0x18), rd(mgr + 0x1c));
						}
					}
				}
				// Emulate PowerPC `blr` (branch to link register) on null
				// dispatch so the thread keeps processing PPC code instead of
				// getting stranded. (Band-aid — the skipped function's side
				// effects are lost, which is why a cascade of nulls can end in
				// an OOB. The real fix is making the pointer non-null.)
				const u32 new_cia = static_cast<u32>(lr) & ~3u;
				printf("[exec_task] thread null-fn at cia=0x%x → blr-emulate to lr=0x%x\n", cia, new_cia);
				cia = new_cia;
				continue;
			}
		}
#endif
		fn->fn(*this, {*op}, op, state ? &ppu_ret : fn + 1);
	}
}

ppu_thread::~ppu_thread()
{
}

ppu_thread::ppu_thread(const ppu_thread_params& param, std::string_view name, u32 _prio, int detached)
	: cpu_thread(idm::last_id())
	, stack_size(param.stack_size)
	, stack_addr(param.stack_addr)
	, joiner(detached != 0 ? ppu_join_status::detached : ppu_join_status::joinable)
	, entry_func(param.entry)
	, start_time(get_guest_system_time())
	, is_interrupt_thread(detached < 0)
	, ppu_tname(make_single<std::string>(name))
{
	prio.raw().prio = _prio;

	memset(&hv_ctx, 0, sizeof(hv_ctx));

	gpr[1] = stack_addr + stack_size - ppu_stack_start_offset;

	gpr[13] = param.tls_addr;

	if (detached >= 0)
	{
		// Initialize thread args
		gpr[3] = param.arg0;
		gpr[4] = param.arg1;
	}

	optional_savestate_state = std::make_shared<utils::serial>();

	// Trigger the scheduler
	state += cpu_flag::suspend;

	// Acquire memory passive lock
	state += cpu_flag::memory;

	call_history.data.resize(g_cfg.core.ppu_call_history ? call_history_max_size : 1);
	syscall_history.data.resize(g_cfg.core.ppu_call_history ? syscall_history_max_size : 1);
	syscall_history.count_debug_arguments = static_cast<u32>(g_cfg.core.ppu_call_history ? std::size(syscall_history.data[0].args) : 0);

#ifdef __APPLE__
	pthread_jit_write_protect_np(true);
#endif
#ifdef ARCH_ARM64
	// Flush all cache lines after potentially writing executable code
	asm("ISB");
	asm("DSB ISH");
#endif
}

struct disable_precomp_t
{
	atomic_t<bool> disable = false;
};

void vdecEntry(ppu_thread& ppu, u32 vid);

bool ppu_thread::savable() const
{
	if (joiner == ppu_join_status::exited)
	{
		return false;
	}

	if (cia == g_fxo->get<ppu_function_manager>().func_addr(FIND_FUNC(vdecEntry)))
	{
		// Do not attempt to save the state of HLE VDEC threads
		return false;
	}

	return true;
}

void ppu_thread::serialize_common(utils::serial& ar)
{
	[[maybe_unused]] const s32 version = GET_OR_USE_SERIALIZATION_VERSION(ar.is_writing(), ppu);

	ar(gpr, fpr, cr, fpscr.bits, lr, ctr, vrsave, cia, xer, sat, nj, prio.raw().all);

	if (cia % 4 || (cia >> 28) >= 0xCu)
	{
		fmt::throw_exception("Failed to serialize PPU thread ID=0x%x (cia=0x%x, ar=%s)", this->id, cia, ar);
	}

	ar(optional_savestate_state, vr);

	if (!ar.is_writing())
	{
		if (optional_savestate_state->data.empty())
		{
			optional_savestate_state->clear();
		}

		optional_savestate_state->set_reading_state();
	}
}

struct save_lv2_tag
{
	atomic_t<bool> saved = false;
	atomic_t<bool> loaded = false;
};

ppu_thread::ppu_thread(utils::serial& ar)
	: cpu_thread(idm::last_id()) // last_id() is showed to constructor on serialization
	, stack_size(ar)
	, stack_addr(ar)
	, joiner(ar.pop<ppu_join_status>())
	, entry_func(std::bit_cast<ppu_func_opd_t, u64>(ar))
	, is_interrupt_thread(ar)
{
	[[maybe_unused]] const s32 version = GET_SERIALIZATION_VERSION(ppu);

	struct init_pushed
	{
		bool pushed = false;
		atomic_t<u32> inited = false;
	};

	call_history.data.resize(g_cfg.core.ppu_call_history ? call_history_max_size : 1);
	syscall_history.data.resize(g_cfg.core.ppu_call_history ? syscall_history_max_size : 1);
	syscall_history.count_debug_arguments = static_cast<u32>(g_cfg.core.ppu_call_history ? std::size(syscall_history.data[0].args) : 0);

	if (version >= 2 && !g_fxo->get<save_lv2_tag>().loaded.exchange(true))
	{
		ar(lv2_obj::g_priority_order_tag);
	}

	if (version >= 3)
	{
		// Function and module for HLE function relocation
		// TODO: Use it
		ar.pop<std::string>();
		ar.pop<std::string>();
	}

	serialize_common(ar);

	// Restore jm_mask
	jm_mask = nj ? 0x7F800000 : 0x7fff'ffff;

	auto queue_intr_entry = [&]()
	{
		if (is_interrupt_thread)
		{
			void ppu_interrupt_thread_entry(ppu_thread&, ppu_opcode_t, be_t<u32>*, struct ppu_intrp_func*);

			cmd_list
			({
				{ ppu_cmd::ptr_call, 0 },
				static_cast<u64>(std::bit_cast<uptr>(&ppu_interrupt_thread_entry))
			});
		}
	};

	const u32 status = ar.pop<u32>();

	switch (status)
	{
	case PPU_THREAD_STATUS_IDLE:
	{
		stop_flag_removal_protection = true;
		break;
	}
	case PPU_THREAD_STATUS_RUNNABLE:
	case PPU_THREAD_STATUS_ONPROC:
	{
		if (version >= 2)
		{
			const u32 order = ar.pop<u32>();

			struct awake_pushed
			{
				bool pushed = false;
				shared_mutex dummy;
				std::map<u32, ppu_thread*> awake_ppus;
			};

			g_fxo->get<awake_pushed>().awake_ppus[order] = this;

			if (!std::exchange(g_fxo->get<awake_pushed>().pushed, true))
			{
				Emu.PostponeInitCode([this]()
				{
					u32 prev = umax;

					for (auto ppu : g_fxo->get<awake_pushed>().awake_ppus)
					{
						ensure(prev + 1 == ppu.first);
						prev = ppu.first;
						lv2_obj::awake(ppu.second);
					}

					g_fxo->get<awake_pushed>().awake_ppus.clear();
				});
			}
		}
		else
		{
			lv2_obj::awake(this);
		}

		[[fallthrough]];
	}
	case PPU_THREAD_STATUS_SLEEP:
	{
		if (std::exchange(g_fxo->get<init_pushed>().pushed, true))
		{
			cmd_list
			({
				{ppu_cmd::ptr_call, 0}, +[](ppu_thread&) -> bool
				{
					while (!Emu.IsStopped() && !g_fxo->get<init_pushed>().inited)
					{
						thread_ctrl::wait_on(g_fxo->get<init_pushed>().inited, 0);
					}
					return false;
				}
			});
		}
		else
		{
			g_fxo->init<disable_precomp_t>();
			g_fxo->get<disable_precomp_t>().disable = true;

			cmd_push({ppu_cmd::initialize, 0});
			cmd_list
			({
				{ppu_cmd::ptr_call, 0}, +[](ppu_thread&) -> bool
				{
					auto& inited = g_fxo->get<init_pushed>().inited;
					inited = 1;
					inited.notify_all();
					return true;
				}
			});
		}

		if (status == PPU_THREAD_STATUS_SLEEP)
		{
			cmd_list
			({
				{ppu_cmd::ptr_call, 0},

				+[](ppu_thread& ppu) -> bool
				{
					const u32 op = vm::read32(ppu.cia);
					const auto& table = g_fxo->get<ppu_interpreter_rt>();
					ppu.loaded_from_savestate = true;
					ppu.prio.raw().preserve_bit = 1;
					table.decode(op)(ppu, {op}, vm::_ptr<u32>(ppu.cia), &ppu_ret);
					ppu.prio.raw().preserve_bit = 0;

					ppu.optional_savestate_state->clear(); // Reset to writing state
					ppu.loaded_from_savestate = false;
					return true;
				}
			});

			lv2_obj::set_future_sleep(this);
		}

		queue_intr_entry();
		cmd_push({ppu_cmd::cia_call, 0});
		break;
	}
	case PPU_THREAD_STATUS_ZOMBIE:
	{
		state += cpu_flag::exit;
		break;
	}
	case PPU_THREAD_STATUS_STOP:
	{
		queue_intr_entry();
		break;
	}
	}

	// Trigger the scheduler
	state += cpu_flag::suspend;
	state += cpu_flag::memory;

	ppu_tname = make_single<std::string>(ar.pop<std::string>());

	ppu_log.notice("Loading PPU Thread [0x%x: %s]: cia=0x%x, state=%s, status=%s", id, *ppu_tname.load(), cia, +state, ppu_thread_status{status});
}

void ppu_thread::save(utils::serial& ar)
{
	// For debugging purposes, load this as soon as this function enters
	const bs_t<cpu_flag> state_flags = state;

	USING_SERIALIZATION_VERSION(ppu);

	const u64 entry = std::bit_cast<u64>(entry_func);

	ppu_join_status _joiner = joiner;
	if (_joiner >= ppu_join_status::max)
	{
		// Joining thread should recover this member properly
		_joiner = ppu_join_status::joinable;
	}

	ar(stack_size, stack_addr, _joiner, entry, is_interrupt_thread);

	const bool is_null = ar.m_file_handler && ar.m_file_handler->is_null();

	if (!is_null && !g_fxo->get<save_lv2_tag>().saved.exchange(true))
	{
		ar(lv2_obj::g_priority_order_tag);
	}

	if (current_module && current_module[0])
	{
		ar(std::string{current_module});
		ar(std::string{last_function});
	}
	else
	{
		ar(std::string{});
		ar(std::string{});
	}

	serialize_common(ar);

	auto [status, order] = lv2_obj::ppu_state(this, false);

	if (status == PPU_THREAD_STATUS_SLEEP && cpu_flag::again - state)
	{
		// Hack for sys_fs
		status = PPU_THREAD_STATUS_RUNNABLE;
	}

	ar(status);

	if (status == PPU_THREAD_STATUS_RUNNABLE || status == PPU_THREAD_STATUS_ONPROC)
	{
		ar(order);
	}

	ar(*ppu_tname.load());

	if (current_module && current_module[0])
	{
		ppu_log.notice("Saving PPU Thread [0x%x: %s]: cia=0x%x, state=%s, statu=%s (at function: %s)", id, *ppu_tname.load(), cia, state_flags, ppu_thread_status{status}, last_function);
	}
	else
	{
		ppu_log.notice("Saving PPU Thread [0x%x: %s]: cia=0x%x, state=%s, statu=%s", id, *ppu_tname.load(), cia, state_flags, ppu_thread_status{status});
	}
}

ppu_thread::thread_name_t::operator std::string() const
{
	std::string thread_name = fmt::format("PPU[0x%x]", _this->id);

	if (const std::string name = *_this->ppu_tname.load(); !name.empty())
	{
		fmt::append(thread_name, " %s", name);
	}

	return thread_name;
}

void ppu_thread::cmd_push(cmd64 cmd)
{
	// Reserve queue space
	const u32 pos = cmd_queue.push_begin();

	// Write single command
	cmd_queue[pos] = cmd;
}

void ppu_thread::cmd_list(std::initializer_list<cmd64> list)
{
	// Reserve queue space
	const u32 pos = cmd_queue.push_begin(static_cast<u32>(list.size()));

	// Write command tail in relaxed manner
	for (u32 i = 1; i < list.size(); i++)
	{
		cmd_queue[pos + i].raw() = list.begin()[i];
	}

	// Write command head after all
	cmd_queue[pos] = *list.begin();
}

void ppu_thread::cmd_pop(u32 count)
{
	// Get current position
	const u32 pos = cmd_queue.peek();

	// Clean command buffer for command tail
	for (u32 i = 1; i <= count; i++)
	{
		cmd_queue[pos + i].raw() = cmd64{};
	}

	// Free
	cmd_queue.pop_end(count + 1);
}

cmd64 ppu_thread::cmd_wait()
{
	while (true)
	{
		if (cmd64 result = cmd_queue[cmd_queue.peek()].exchange(cmd64{}))
		{
			return result;
		}

		if (is_stopped())
		{
			return {};
		}

		thread_ctrl::wait_on(cmd_notify, 0);
		cmd_notify = 0;
	}
}

be_t<u64>* ppu_thread::get_stack_arg(s32 i, u64 align)
{
	if (align != 1 && align != 2 && align != 4 && align != 8 && align != 16) fmt::throw_exception("Unsupported alignment: 0x%llx", align);
	return vm::_ptr<u64>(vm::cast((gpr[1] + 0x30 + 0x8 * (i - 1)) & (0 - align)));
}

void ppu_thread::fast_call(u32 addr, u64 rtoc, bool is_thread_entry)
{
	const auto old_cia = cia;
	const auto old_rtoc = gpr[2];
	const auto old_lr = lr;
	const auto old_func = current_function;
	const auto old_fmt = g_tls_log_prefix;

	interrupt_thread_executing = true;
	cia = addr;
	gpr[2] = rtoc;
	lr = g_fxo->get<ppu_function_manager>().func_addr(1, true); // HLE stop address
	current_function = nullptr;

	if (std::exchange(loaded_from_savestate, false))
	{
		lr = old_lr;
	}

	g_tls_log_prefix = []
	{
		const auto _this = static_cast<ppu_thread*>(get_current_cpu_thread());

		static thread_local shared_ptr<std::string> name_cache;

		if (!_this->ppu_tname.is_equal(name_cache)) [[unlikely]]
		{
			_this->ppu_tname.peek_op([&](const shared_ptr<std::string>& ptr)
			{
				if (ptr != name_cache)
				{
					name_cache = ptr;
				}
			});
		}

		const auto cia = _this->cia;

		if (_this->current_function && g_fxo->get<ppu_function_manager>().is_func(cia))
		{
			return fmt::format("PPU[0x%x] Thread (%s) [HLE:0x%08x, LR:0x%08x]", _this->id, *name_cache.get(), cia, _this->lr);
		}

		extern const char* get_prx_name_by_cia(u32 addr);

		if (auto name = get_prx_name_by_cia(cia))
		{
			return fmt::format("PPU[0x%x] Thread (%s) [%s: 0x%08x]", _this->id, *name_cache.get(), name, cia);
		}

		return fmt::format("PPU[0x%x] Thread (%s) [0x%08x]", _this->id, *name_cache.get(), cia);
	};

	auto at_ret = [&]()
	{
		if (old_cia)
		{
			if (state & cpu_flag::again)
			{
				ppu_log.error("HLE callstack savestate is not implemented!");
			}

			cia = old_cia;
			gpr[2] = old_rtoc;
			lr = old_lr;
		}
		else if (state & cpu_flag::ret && cia == g_fxo->get<ppu_function_manager>().func_addr(1, true) + 4 && is_thread_entry)
		{
#if defined(__EMSCRIPTEN__)
			// wasm32: for HLE-redirected thread entries (see
			// sys_ppu_thread.cpp's HLE-region redirect to HLE_RETURN),
			// reaching here is the EXPECTED clean exit, not an error. The
			// native dump_all() path calls into PPUDisAsm + std::any +
			// fmt::append-of-vm::check_addr — at least one of those traps
			// `unreachable` on wasm32 (seen in browser console: stack ends
			// at ppu_thread::dump_regs → cpu_thread::dump_all). Skip the
			// dump and just log a short notice so the worker exits cleanly.
			ppu_log.notice("Thread entry returned via HLE_RETURN (func=0x%x) — wasm32 redirect", entry_func.addr);
#else
			std::string ret;
			dump_all(ret);

			ppu_log.error("Returning from the thread entry function! (func=0x%x)", entry_func.addr);
			ppu_log.notice("Thread context: %s", ret);
#endif

			lv2_obj::sleep(*this);

			// For savestates
			state += cpu_flag::again;
			std::memcpy(syscall_args, &gpr[3], sizeof(syscall_args));
		}

		if (!old_cia && state & cpu_flag::again)
		{
			// Fixup argument registers and CIA for reloading
			std::memcpy(&gpr[3], syscall_args, sizeof(syscall_args));
			cia -= 4;
		}

		current_function = old_func;
		g_tls_log_prefix = old_fmt;
		state -= cpu_flag::ret;
	};

	exec_task();
	at_ret();
}

std::pair<vm::addr_t, u32> ppu_thread::stack_push(u32 size, u32 align_v)
{
	if (auto cpu = get_current_cpu_thread<ppu_thread>())
	{
		ppu_thread& context = static_cast<ppu_thread&>(*cpu);

		const u32 old_pos = vm::cast(context.gpr[1]);
		context.gpr[1] -= size; // room minimal possible size
		context.gpr[1] &= ~(u64{align_v} - 1); // fix stack alignment

		auto is_stack = [&](u64 addr)
		{
			return addr >= context.stack_addr && addr < context.stack_addr + context.stack_size;
		};

		// TODO: This check does not care about custom stack memory
		if (is_stack(old_pos) != is_stack(context.gpr[1]))
		{
			fmt::throw_exception("Stack overflow (size=0x%x, align=0x%x, SP=0x%llx, stack=*0x%x)", size, align_v, old_pos, context.stack_addr);
		}
		else
		{
			const u32 addr = static_cast<u32>(context.gpr[1]);
			std::memset(vm::base(addr), 0, size);
			return {vm::cast(addr), old_pos - addr};
		}
	}

	fmt::throw_exception("Invalid thread");
}

void ppu_thread::stack_pop_verbose(u32 addr, u32 size) noexcept
{
	if (auto cpu = get_current_cpu_thread<ppu_thread>())
	{
		ppu_thread& context = static_cast<ppu_thread&>(*cpu);

		if (context.gpr[1] != addr)
		{
			ppu_log.error("Stack inconsistency (addr=0x%x, SP=0x%llx, size=0x%x)", addr, context.gpr[1], size);
			return;
		}

		context.gpr[1] += size;
		return;
	}

	ppu_log.error("Invalid thread");
}

extern ppu_intrp_func_t ppu_get_syscall(u64 code);

void ppu_trap(ppu_thread& ppu, u64 addr)
{
	ensure((addr & (~u64{0xffff'ffff} | 0x3)) == 0);
	ppu.cia = static_cast<u32>(addr);

	u32 add = static_cast<u32>(g_cfg.core.stub_ppu_traps) * 4;

	// If stubbing is enabled, check current instruction and the following
	if (!add || !vm::check_addr(ppu.cia, vm::page_executable) || !vm::check_addr(ppu.cia + add, vm::page_executable))
	{
		fmt::throw_exception("PPU Trap! Sometimes tweaking the setting \"Stub PPU Traps\" can be a workaround to this crash.\nBest values depend on game code, if unsure try 1.");
	}

	ppu_log.error("PPU Trap: Stubbing %d instructions %s.", std::abs(static_cast<s32>(add) / 4), add >> 31 ? "backwards" : "forwards");
	ppu.cia += add; // Skip instructions, hope for valid code (interprter may be invoked temporarily)
}

static void ppu_error(ppu_thread& ppu, u64 addr, u32 /*op*/)
{
	ppu.cia = ::narrow<u32>(addr);
	ppu_recompiler_fallback(ppu);
}

static void ppu_check(ppu_thread& ppu, u64 addr)
{
	ppu.cia = ::narrow<u32>(addr);

	if (ppu.test_stopped())
	{
		return;
	}
}

static void ppu_trace(u64 addr)
{
	ppu_log.notice("Trace: 0x%llx", addr);
}

template <typename T>
static T ppu_load_acquire_reservation(ppu_thread& ppu, u32 addr)
{
	perf_meter<"LARX"_u32> perf0;

	// Do not allow stores accessed from the same cache line to past reservation load
	atomic_fence_seq_cst();

	if (addr % sizeof(T))
	{
		fmt::throw_exception("PPU %s: Unaligned address: 0x%08x", sizeof(T) == 4 ? "LWARX" : "LDARX", addr);
	}

	// Always load aligned 64-bit value
	auto& data = vm::_ref<const atomic_be_t<u64>>(addr & -8);
	const u64 size_off = (sizeof(T) * 8) & 63;
	const u64 data_off = (addr & 7) * 8;

	ppu.raddr = addr;

	u32 addr_mask = -1;

	if (const s32 max = g_cfg.core.ppu_128_reservations_loop_max_length)
	{
		// If we use it in HLE it means we want the accurate version
		ppu.use_full_rdata = max < 0 || ppu.current_function || [&]()
		{
			const u32 cia = ppu.cia;

			if ((cia & 0xffff) >= 0x10000u - max * 4)
			{
				// Do not cross 64k boundary
				return false;
			}

			const auto inst = vm::_ptr<const nse_t<u32>>(cia);

			// Search for STWCX or STDCX nearby (LDARX-STWCX and LWARX-STDCX loops will use accurate 128-byte reservations)
			constexpr u32 store_cond = stx::se_storage<u32>::swap(sizeof(T) == 8 ? 0x7C00012D : 0x7C0001AD);
			constexpr u32 mask = stx::se_storage<u32>::swap(0xFC0007FF);

			const auto store_vec = v128::from32p(store_cond);
			const auto mask_vec = v128::from32p(mask);

			s32 i = 2;

			for (const s32 _max = max - 3; i < _max; i += 4)
			{
				const auto _inst = v128::loadu(inst + i) & mask_vec;

				if (!gv_testz(gv_eq32(_inst, store_vec)))
				{
					return false;
				}
			}

			for (; i < max; i++)
			{
				const u32 val = inst[i] & mask;

				if (val == store_cond)
				{
					return false;
				}
			}

			return true;
		}();

		if (ppu.use_full_rdata)
		{
			addr_mask = -128;
		}
	}
	else
	{
		ppu.use_full_rdata = false;
	}

	if (ppu_log.trace && (addr & addr_mask) == (ppu.last_faddr & addr_mask))
	{
		ppu_log.trace(u8"LARX after fail: addr=0x%x, faddr=0x%x, time=%u c", addr, ppu.last_faddr, (perf0.get() - ppu.last_ftsc));
	}

	if ((addr & addr_mask) == (ppu.last_faddr & addr_mask) && (perf0.get() - ppu.last_ftsc) < 600 && (vm::reservation_acquire(addr) & -128) == ppu.last_ftime)
	{
		be_t<u64> rdata;
		std::memcpy(&rdata, &ppu.rdata[addr & 0x78], 8);

		if (rdata == data.load())
		{
			ppu.rtime = ppu.last_ftime;
			ppu.raddr = ppu.last_faddr;
			ppu.last_ftime = 0;
			return static_cast<T>(rdata << data_off >> size_off);
		}

		ppu.last_fail++;
		ppu.last_faddr = 0;
	}
	else
	{
		// Silent failure
		ppu.last_faddr = 0;
	}

	const u32 res_cached = ppu.res_cached;

	if ((addr & -128) == (res_cached & -128))
	{
		// Reload "cached" reservation of previous succeeded conditional store
		// This seems like a hardware feature according to cellSpursAddUrgentCommand function
	}
	else
	{
		ppu.rtime = vm::reservation_acquire(addr) & -128;
	}

	be_t<u64> rdata;

	if (!ppu.use_full_rdata)
	{
		rdata = data.load();

		// Store only 64 bits of reservation data
		std::memcpy(&ppu.rdata[addr & 0x78], &rdata, 8);
	}
	else
	{
		mov_rdata(ppu.rdata, vm::_ref<spu_rdata_t>(addr & -128));
		atomic_fence_acquire();

		// Load relevant 64 bits of reservation data
		std::memcpy(&rdata, &ppu.rdata[addr & 0x78], 8);
	}

	return static_cast<T>(rdata << data_off >> size_off);
}

extern u32 ppu_lwarx(ppu_thread& ppu, u32 addr)
{
	return ppu_load_acquire_reservation<u32>(ppu, addr);
}

extern u64 ppu_ldarx(ppu_thread& ppu, u32 addr)
{
	return ppu_load_acquire_reservation<u64>(ppu, addr);
}

template <typename T>
static bool ppu_store_reservation(ppu_thread& ppu, u32 addr, u64 reg_value)
{
	perf_meter<"STCX"_u32> perf0;

	if (addr % sizeof(T))
	{
		fmt::throw_exception("PPU %s: Unaligned address: 0x%08x", sizeof(T) == 4 ? "STWCX" : "STDCX", addr);
	}

	// Notify breakpoint handler
	vm::write<void>(addr, T{0}, &ppu);

	auto& data = const_cast<atomic_be_t<u64>&>(vm::_ref<atomic_be_t<u64>>(addr & -8));
	auto& res = vm::reservation_acquire(addr);
	const u64 rtime = ppu.rtime;

	be_t<u64> old_data = 0;
	std::memcpy(&old_data, &ppu.rdata[addr & 0x78], sizeof(old_data));
	be_t<u64> new_data = old_data;

	if constexpr (sizeof(T) == sizeof(u32))
	{
		// Rebuild reg_value to be 32-bits of new data and 32-bits of old data
		const be_t<u32> reg32 = static_cast<u32>(reg_value);
		std::memcpy(reinterpret_cast<char*>(&new_data) + (addr & 4), &reg32, sizeof(u32));
	}
	else
	{
		new_data = reg_value;
	}

	// Test if store address is on the same aligned 8-bytes memory as load
	if (const u32 raddr = ppu.raddr; raddr / 8 != addr / 8)
	{
		// If not and it is on the same aligned 128-byte memory, proceed only if 128-byte reservations are enabled
		// In realhw the store address can be at any address of the 128-byte cache line
		if (raddr / 128 != addr / 128 || !ppu.use_full_rdata)
		{
			// Even when the reservation address does not match the target address must be valid
			if (!vm::check_addr(addr, vm::page_writable))
			{
				// Access violate
				data += 0;
			}

			ppu.raddr = 0;
			ppu.res_cached = 0;
			return false;
		}
	}

	if (old_data != data || rtime != (res & -128))
	{
		ppu.raddr = 0;
		ppu.res_cached = 0;
		return false;
	}

	if ([&]()
	{
		if (ppu.use_full_rdata) [[unlikely]]
		{
			auto [_oldd, _ok] = res.fetch_op([&](u64& r)
			{
				if ((r & -128) != rtime || (r & 127))
				{
					return false;
				}

				r += vm::rsrv_unique_lock;
				return true;
			});

			if (!_ok)
			{
				// Already locked or updated: give up
				return false;
			}

			// Align address: we do not need the lower 7 bits anymore
			addr &= -128;

			// Cache line data
			//auto& cline_data = vm::_ref<spu_rdata_t>(addr);

			data += 0;
			auto range_lock = vm::alloc_range_lock();
			bool success = false;
			{
				rsx::reservation_lock rsx_lock(addr, 128);

				auto& super_data = *vm::get_super_ptr<spu_rdata_t>(addr);
				success = [&]()
				{
					// Full lock (heavyweight)
					// TODO: vm::check_addr
					vm::writer_lock lock(addr, range_lock);

					if (cmp_rdata(ppu.rdata, super_data))
					{
						data.release(new_data);
						res += 64;
						return true;
					}

					res -= 64;
					return false;
				}();
			}
			vm::free_range_lock(range_lock);

			return success;
		}

		if (new_data == old_data)
		{
			ppu.last_faddr = 0;
			return res.compare_and_swap_test(rtime, rtime + 128);
		}

		// Aligned 8-byte reservations will be used here
		addr &= -8;

		const u64 lock_bits = vm::rsrv_unique_lock;

		auto [_oldd, _ok] = res.fetch_op([&](u64& r)
		{
			if ((r & -128) != rtime || (r & 127))
			{
				return false;
			}

			r += lock_bits;
			return true;
		});

		// Give up if reservation has been locked or updated
		if (!_ok)
		{
			ppu.last_faddr = 0;
			return false;
		}

		// Store previous value in old_data on failure
		if (data.compare_exchange(old_data, new_data))
		{
			res += 128 - lock_bits;
			return true;
		}

		const u64 old_rtime = res.fetch_sub(lock_bits);

		// TODO: disabled with this setting on, since it's dangerous to mix
		if (!g_cfg.core.ppu_128_reservations_loop_max_length)
		{
			// Store old_data on failure
			if (ppu.last_faddr == addr)
			{
				ppu.last_fail++;
			}

			ppu.last_faddr = addr;
			ppu.last_ftime = old_rtime & -128;
			ppu.last_ftsc = utils::get_tsc();
			std::memcpy(&ppu.rdata[addr & 0x78], &old_data, 8);
		}

		return false;
	}())
	{
		extern atomic_t<u32> liblv2_begin, liblv2_end;

		u32 notify = ppu.res_notify;

		// Avoid notifications from lwmutex or sys_spinlock
		const bool is_liblv2_or_null = (ppu.cia >= liblv2_begin && ppu.cia < liblv2_end);

		if (!is_liblv2_or_null)
		{
			// Try to postpone notification to when PPU is asleep or join notifications on the same address
			// This also optimizes a mutex - won't notify after lock is aqcuired (prolonging the critical section duration), only notifies on unlock
			const u32 count = vm::reservation_notifier_count(addr, rtime);

			switch (count)
			{
			case 0:
			{
				// Nothing to do
				break;
			}
			case 1:
			{
				// Postpone notifications if there is no pending one OR if there is likely a complex operation on reservation going on
				// Which consists of multiple used addresses
				if (ppu.res_notify_postpone_streak <= 4)
				{
					if (!notify || ((notify & -128) == (addr & -128) && new_data != old_data))
					{
						ppu.res_notify = addr;
						ppu.res_notify_time = rtime;
						ppu.res_notify_postpone_streak++;
						notify = 0;
						break;
					}
				}

				// Notify both
				[[fallthrough]];
			}
			default:
			{
				if (cpu_flag::wait - ppu.state)
				{
					ppu.state += cpu_flag::wait;
				}

				vm::reservation_notifier_notify(addr, rtime);
				break;
			}
			}
		}

		if (notify)
		{
			if (auto waiter = vm::reservation_notifier_notify(notify, ppu.res_notify_time, true))
			{
				ppu.state += cpu_flag::wait;
				waiter->notify_all();
			}

			ppu.res_notify = 0;
			ppu.res_notify_postpone_streak = 0;
		}

		static_cast<void>(ppu.test_stopped());

		if (addr == ppu.last_faddr)
		{
			ppu.last_succ++;
		}

		ppu.last_faddr = 0;
		ppu.res_cached = ppu.raddr;
		ppu.raddr = 0;
		return true;
	}

	const u32 notify = ppu.res_notify;

	// Do not risk postponing too much (because this is probably an indefinite loop)
	// And on failure it has some time to do something else
	if (notify && ((addr ^ notify) & -128))
	{
		if (auto waiter = vm::reservation_notifier_notify(notify, ppu.res_notify_time, true))
		{
			ppu.state += cpu_flag::wait;
			waiter->notify_all();
			static_cast<void>(ppu.test_stopped());
		}

		ppu.res_notify = 0;
		ppu.res_notify_postpone_streak = 0;
	}

	ppu.raddr = 0;
	ppu.res_cached = 0;
	return false;
}

extern bool ppu_stwcx(ppu_thread& ppu, u32 addr, u32 reg_value)
{
	return ppu_store_reservation<u32>(ppu, addr, reg_value);
}

extern bool ppu_stdcx(ppu_thread& ppu, u32 addr, u64 reg_value)
{
	return ppu_store_reservation<u64>(ppu, addr, reg_value);
}

struct jit_core_allocator
{
	const s16 thread_count = g_cfg.core.llvm_threads ? std::min<s32>(g_cfg.core.llvm_threads, limit()) : limit();

	// Initialize global semaphore with the max number of threads
	::semaphore<0x7fff> sem{std::max<s16>(thread_count, 1)};

	// Mutex for special extra-large modules to compile alone
	shared_mutex shared_mtx;

	static s16 limit()
	{
		return static_cast<s16>(std::min<s32>(0x7fff, utils::get_thread_count()));
	}
};

#ifdef LLVM_AVAILABLE
namespace
{
	// Compiled PPU module info
	struct jit_module
	{
		std::vector<void(*)(u8*, u64)> symbol_resolvers;
		std::vector<std::shared_ptr<jit_compiler>> pjit;
		bool init = false;
	};

	struct jit_module_manager
	{
		struct bucket_t
		{
			shared_mutex mutex;
			std::unordered_map<std::string, jit_module> map;
		};

		std::array<bucket_t, 30> buckets;

		bucket_t& get_bucket(std::string_view sv)
		{
			return buckets[std::hash<std::string_view>()(sv) % std::size(buckets)];
		}

		jit_module& get(const std::string& name)
		{
			bucket_t& bucket = get_bucket(name);
			std::lock_guard lock(bucket.mutex);
			return bucket.map.emplace(name, jit_module{}).first->second;
		}

		void remove(const std::string& name) noexcept
		{
			bucket_t& bucket = get_bucket(name);

			jit_module to_destroy{};

			std::lock_guard lock(bucket.mutex);
			const auto found = bucket.map.find(name);

			if (found == bucket.map.end()) [[unlikely]]
			{
				ppu_log.error("Failed to remove module %s", name);

				for (auto& buck : buckets)
				{
					for (auto& mod : buck.map)
					{
						ppu_log.notice("But there is module %s", mod.first);
					}
				}

				return;
			}

			to_destroy.pjit = std::move(found->second.pjit);
			to_destroy.symbol_resolvers = std::move(found->second.symbol_resolvers);

			bucket.map.erase(found);
		}

		jit_module_manager& operator=(thread_state s) noexcept
		{
			if (s == thread_state::destroying_context)
			{
				for (auto& buck : buckets)
				{
					for (auto& mod : buck.map)
					{
						for (auto& jit : mod.second.pjit)
						{
							*jit = s;
						}
					}
				}
			}

			return *this;
		}
	};
}
#endif

namespace
{
	// Read-only file view starting with specified offset (for MSELF)
	struct file_view : fs::file_base
	{
		const fs::file m_storage;
		const fs::file& m_file;
		const u64 m_off;
		const u64 m_max_size;
		u64 m_pos;

		explicit file_view(const fs::file& _file, u64 offset, u64 max_size) noexcept
			: m_storage(fs::file())
			, m_file(_file)
			, m_off(offset)
			, m_max_size(max_size)
			, m_pos(0)
		{
		}

		explicit file_view(fs::file&& _file, u64 offset, u64 max_size) noexcept
			: m_storage(std::move(_file))
			, m_file(m_storage)
			, m_off(offset)
			, m_max_size(max_size)
			, m_pos(0)
		{
		}

		~file_view() override
		{
		}

		fs::stat_t get_stat() override
		{
			fs::stat_t stat = m_file.get_stat();
			stat.size = std::min<u64>(utils::sub_saturate<u64>(stat.size, m_off), m_max_size);
			stat.is_writable = false;
			return stat;
		}

		bool trunc(u64) override
		{
			return false;
		}

		u64 read(void* buffer, u64 size) override
		{
			const u64 result = file_view::read_at(m_pos, buffer, size);
			m_pos += result;
			return result;
		}

		u64 read_at(u64 offset, void* buffer, u64 size) override
		{
			return m_file.read_at(offset + m_off, buffer, std::min<u64>(size, utils::sub_saturate<u64>(m_max_size, offset)));
		}

		u64 write(const void*, u64) override
		{
			return 0;
		}

		u64 seek(s64 offset, fs::seek_mode whence) override
		{
			const s64 new_pos =
				whence == fs::seek_set ? offset :
				whence == fs::seek_cur ? offset + m_pos :
				whence == fs::seek_end ? offset + size() : -1;

			if (new_pos < 0)
			{
				fs::g_tls_error = fs::error::inval;
				return -1;
			}

			m_pos = new_pos;
			return m_pos;
		}

		u64 size() override
		{
			return std::min<u64>(utils::sub_saturate<u64>(m_file.size(), m_off), m_max_size);
		}
	};
}

extern fs::file make_file_view(const fs::file& _file, u64 offset, u64 max_size = umax)
{
	fs::file file;
	file.reset(std::make_unique<file_view>(_file, offset, max_size));
	return file;
}

extern fs::file make_file_view(fs::file&& _file, u64 offset, u64 max_size = umax)
{
	fs::file file;
	file.reset(std::make_unique<file_view>(std::move(_file), offset, max_size));
	return file;
}

extern void ppu_finalize(const ppu_module<lv2_obj>& info, bool force_mem_release)
{
	if (info.segs.empty())
	{
		// HLEd modules
		return;
	}

	if (!force_mem_release && info.name.empty())
	{
		// Don't remove main module from memory
		return;
	}

	if (!force_mem_release && Emu.GetCat() == "1P")
	{
		return;
	}

	const bool may_be_elf = fmt::to_lower(info.path.substr(std::max<usz>(info.path.size(), 3) - 3)) != "prx";

	const std::string dev_flash = vfs::get("/dev_flash/");

	if (!may_be_elf)
	{
		if (!force_mem_release && info.path.starts_with(dev_flash + "sys/external/"))
		{
			// Don't remove dev_flash prx from memory
			return;
		}
	}

	if (g_cfg.core.ppu_decoder != ppu_decoder_type::llvm)
	{
		return;
	}

	// Get cache path for this executable
	std::string cache_path = rpcs3::utils::get_cache_dir(info.path);

	// Add PPU hash and filename
	fmt::append(cache_path, "ppu-%s-%s/", fmt::base57(info.sha1), info.path.substr(info.path.find_last_of('/') + 1));

#ifdef LLVM_AVAILABLE
	g_fxo->get<jit_module_manager>().remove(cache_path + "_" + std::to_string(std::bit_cast<usz>(info.segs[0].ptr)));
#endif
}

extern void ppu_precompile(std::vector<std::string>& dir_queue, std::vector<ppu_module<lv2_obj>*>* loaded_modules, bool is_fast_compilation)
{
	if (g_cfg.core.ppu_decoder != ppu_decoder_type::llvm)
	{
		return;
	}

	if (auto dis = g_fxo->try_get<disable_precomp_t>(); dis && dis->disable)
	{
		return;
	}

	std::optional<scoped_progress_dialog> progress_dialog(std::in_place, get_localized_string(localized_string_id::PROGRESS_DIALOG_SCANNING_PPU_EXECUTABLE));

	// Make sure we only have one '/' at the end and remove duplicates.
	for (std::string& dir : dir_queue)
	{
		while (dir.back() == '/' || dir.back() == '\\')
			dir.pop_back();
		dir += '/';
	}

	std::sort(dir_queue.begin(), dir_queue.end());
	dir_queue.erase(std::unique(dir_queue.begin(), dir_queue.end()), dir_queue.end());

	const std::string firmware_sprx_path = vfs::get("/dev_flash/sys/external/");

	struct file_info
	{
		std::string path;
		u64 offset;
		u64 file_size;

		file_info() noexcept = default;

		file_info(std::string _path, u64 offs, u64 size) noexcept
			: path(std::move(_path))
			, offset(offs)
			, file_size(size)
		{
		}
	};

	std::vector<file_info> file_queue;
	file_queue.reserve(2000);

	// Find all .sprx files recursively
	for (usz i = 0; i < dir_queue.size(); i++)
	{
		if (Emu.IsStopped())
		{
			file_queue.clear();
			break;
		}

		ppu_log.notice("Scanning directory: %s", dir_queue[i]);

		for (auto&& entry : fs::dir(dir_queue[i]))
		{
			if (Emu.IsStopped())
			{
				file_queue.clear();
				break;
			}

			if (entry.is_directory)
			{
				if (entry.name != "." && entry.name != "..")
				{
					dir_queue.emplace_back(dir_queue[i] + entry.name + '/');
				}

				continue;
			}

			// SCE header size
			if (entry.size <= 0x20)
			{
				continue;
			}

			std::string upper = fmt::to_upper(entry.name);

			// Skip already loaded modules or HLEd ones
			auto is_ignored = [&](s64 /*offset*/) -> bool
			{
				if (dir_queue[i] != firmware_sprx_path)
				{
					return false;
				}

				if (loaded_modules)
				{
					if (std::any_of(loaded_modules->begin(), loaded_modules->end(), [&](ppu_module<lv2_obj>* obj)
					{
						return obj->name == entry.name;
					}))
					{
						return true;
					}
				}

				if (g_cfg.core.libraries_control.get_set().count(entry.name + ":lle"))
				{
					// Force LLE
					return false;
				}
				else if (g_cfg.core.libraries_control.get_set().count(entry.name + ":hle"))
				{
					// Force HLE
					return true;
				}

				extern const std::map<std::string_view, int> g_prx_list;

				// Use list
				return g_prx_list.count(entry.name) && ::at32(g_prx_list, entry.name) != 0;
			};

			// Check PRX filename
			if (upper.ends_with(".PRX") || (upper.ends_with(".SPRX") && entry.name != "libfs_utility_init.sprx"sv))
			{
				if (is_ignored(0))
				{
					continue;
				}

				// Get full path
				file_queue.emplace_back(dir_queue[i] + entry.name, 0, entry.size);
				continue;
			}

			// Check ELF filename
			if ((upper.ends_with(".ELF") || upper.ends_with(".SELF")) && Emu.GetBoot() != dir_queue[i] + entry.name)
			{
				// Get full path
				file_queue.emplace_back(dir_queue[i] + entry.name, 0, entry.size);
				continue;
			}

			// Check .mself filename
			if (upper.ends_with(".MSELF"))
			{
				if (fs::file mself{dir_queue[i] + entry.name})
				{
					mself_header hdr{};

					if (mself.read(hdr) && hdr.get_count(mself.size()))
					{
						std::set<u64> offs;

						for (u32 j = 0; j < hdr.count; j++)
						{
							mself_record rec{};

							if (mself.read(rec) && rec.get_pos(mself.size()))
							{
								if (rec.size <= 0x20)
								{
									continue;
								}

								if (!offs.emplace(rec.off).second)
								{
									// Duplicate
									continue;
								}

								// Read characters safely
								std::string name(sizeof(rec.name), '\0');

								std::memcpy(name.data(), rec.name, name.size());
								name = std::string(name.c_str());

								upper = fmt::to_upper(name);

								if (upper.find(".SPRX") != umax || upper.find(".PRX") != umax)
								{
									// .sprx inside .mself found
									file_queue.emplace_back(dir_queue[i] + entry.name, rec.off, rec.size);
									continue;
								}

								if (upper.find(".SELF") != umax || upper.find(".ELF") != umax)
								{
									// .self inside .mself found
									file_queue.emplace_back(dir_queue[i] + entry.name, rec.off, rec.size);
									continue;
								}
							}
							else
							{
								ppu_log.error("MSELF file is possibly truncated");
								break;
							}
						}
					}
				}
			}
		}
	}

	g_progr_ftotal += ::size32(file_queue);

	u64 total_files_size = 0;

	for (const file_info& info : file_queue)
	{
		total_files_size += info.file_size;
	}

	g_progr_ftotal_bits += total_files_size;

	*progress_dialog = get_localized_string(localized_string_id::PROGRESS_DIALOG_COMPILING_PPU_MODULES);

	atomic_t<usz> fnext = 0;

	lf_queue<file_info> possible_exec_file_paths;

	// Allow to allocate 2000 times the size of each file for the use of LLVM
	// This works very nicely with Metal Gear Solid 4 for example:
	// 2 7MB overlay files -> 14GB
	// The growth in memory requirements of LLVM is not linear with file size of course
	// But these estimates should hopefully protect RPCS3 in the coming years
	// Especially when thread count is on the rise with each CPU generation
	atomic_t<u32> file_size_limit = static_cast<u32>(std::clamp<u64>(utils::aligned_div<u64>(utils::get_total_memory(), 2000), 65536, u32{umax}));

	const u32 software_thread_limit = std::min<u32>(g_cfg.core.llvm_threads ? g_cfg.core.llvm_threads : u32{umax}, ::size32(file_queue));
	const u32 cpu_thread_limit = utils::get_thread_count() > 8u ? std::max<u32>(utils::get_thread_count(), 2) - 1 : utils::get_thread_count(); // One LLVM thread less

	std::vector<u128> decrypt_klics;

	if (loaded_modules)
	{
		for (auto mod : *loaded_modules)
		{
			for (const auto& [stub, data_vec] : mod->stub_addr_to_constant_state_of_registers)
			{
				if (decrypt_klics.size() >= 4u)
				{
					break;
				}

				for (const auto& [reg_mask, constant_value] : data_vec)
				{
					if (decrypt_klics.size() >= 4u)
					{
						break;
					}

					if (constant_value > u32{umax})
					{
						continue;
					}

					// R3 - first argument
					if (reg_mask.mask & (1u << 3))
					{
						// Sizeof KLIC
						if (auto klic_ptr = mod->get_ptr<const u8>(static_cast<u32>(constant_value), 16))
						{
							// Try to read from that address
							if (const u128 klic_value = read_from_ptr<u128>(klic_ptr))
							{
								if (!std::count_if(decrypt_klics.begin(), decrypt_klics.end(), FN(std::memcmp(&x, &klic_value, 16) == 0)))
								{
									decrypt_klics.emplace_back(klic_value);
								}
							}
						}
					}
				}
			}
		}
	}

	named_thread_group workers("SPRX Worker ", std::min<u32>(software_thread_limit, cpu_thread_limit), [&]
	{
#ifdef __APPLE__
		pthread_jit_write_protect_np(false);
#endif
		// Set low priority
		thread_ctrl::scoped_priority low_prio(-1);
		u32 inc_fdone = 1;
		u32 restore_mem = 0;

		for (usz func_i = fnext++; func_i < file_queue.size(); func_i = fnext++, g_progr_fdone += std::exchange(inc_fdone, 1))
		{
			if (Emu.IsStopped())
			{
				continue;
			}

			if (restore_mem)
			{
				if (!file_size_limit.fetch_add(restore_mem))
				{
					file_size_limit.notify_all();
				}

				restore_mem = 0;
			}

			auto& [path, offset, file_size] = file_queue[func_i];

			ppu_log.notice("Trying to load: %s", path);

			// Load MSELF, SPRX or SELF
			fs::file src{path};

			if (!src)
			{
				ppu_log.error("Failed to open '%s' (%s)", path, fs::g_tls_error);
				continue;
			}

			if (u64 off = offset)
			{
				// Adjust offset for MSELF
				src = make_file_view(std::move(src), offset, file_size);

				// Adjust path for MSELF too
				fmt::append(path, "_x%x", off);
			}

			for (usz i = 0;; i++)
			{
				if (i > decrypt_klics.size())
				{
					src.close();
					break;
				}

				// Some files may fail to decrypt due to the lack of klic
				u128 key = i == decrypt_klics.size() ? u128{} : decrypt_klics[i];

				if (auto result = decrypt_self(src, i == decrypt_klics.size() ? nullptr : reinterpret_cast<const u8*>(&key)))
				{
					src = std::move(result);
					break;
				}
			}

			if (!src && !Emu.klic.empty() && src.open(path))
			{
				src = decrypt_self(src, reinterpret_cast<u8*>(&Emu.klic[0]));

				if (src)
				{
					ppu_log.error("Possible missed KLIC for precompilation of '%s', please report to developers.", path);

					// Ignore executables larger than 500KB to prevent a long pause on exitspawn
					if (src.size() >= 500000)
					{
						g_progr_ftotal_bits -= file_size;

						continue;
					}
				}
			}

			if (!src)
			{
				ppu_log.notice("Failed to decrypt '%s'", path);

				g_progr_ftotal_bits -= file_size;

				continue;
			}

			auto wait_for_memory = [&]() -> bool
			{
				// Try not to process too many files at once because it seems to reduce performance and cause RAM shortages
				// Concurrently compiling more OVL or huge PRX files does not have much theoretical benefit
				while (!file_size_limit.fetch_op([&](u32& value)
				{
					if (value)
					{
						// Allow at least one file, make 0 the "memory unavailable" sign value for atomic waiting efficiency
						const u32 new_val = static_cast<u32>(utils::sub_saturate<u64>(value, file_size));
						restore_mem = value - new_val;
						value = new_val;
						return true;
					}

					// Resort to waiting
					restore_mem = 0;
					return false;
				}).second)
				{
					// Wait until not 0
					file_size_limit.wait(0);
				}

				if (Emu.IsStopped())
				{
					return false;
				}

				return true;
			};

			elf_error prx_err{}, ovl_err{};

			if (ppu_prx_object obj = src; (prx_err = obj, obj == elf_error::ok))
			{
				if (!wait_for_memory())
				{
					// Emulation stopped
					continue;
				}

				if (auto prx = ppu_load_prx(obj, true, path, offset))
				{
					obj.clear(), src.close(); // Clear decrypted file and elf object memory
					ppu_initialize(*prx, false, file_size);
					ppu_finalize(*prx, true);
					continue;
				}

				// Log error
				prx_err = elf_error::header_type;
			}

			if (ppu_exec_object obj = src; (ovl_err = obj, obj == elf_error::ok))
			{
				while (ovl_err == elf_error::ok)
				{
					if (Emu.IsStopped())
					{
						break;
					}

					const auto [ovlm, error] = ppu_load_overlay(obj, true, path, offset);

					if (error)
					{
						if (error == CELL_CANCEL + 0u)
						{
							// Emulation stopped
							break;
						}

						// Abort
						ovl_err = elf_error::header_type;
						break;
					}

					if (is_fast_compilation)
					{
						// Skip overlays in fast mode
						break;
					}

					if (!wait_for_memory())
					{
						// Emulation stopped
						break;
					}

					// Participate in thread execution limitation (takes a long time)
					if (std::lock_guard lock(g_fxo->get<jit_core_allocator>().sem); !ovlm->analyse(0, ovlm->entry, ovlm->seg0_code_end, ovlm->applied_patches, std::vector<u32>{}, []()
					{
						return Emu.IsStopped();
					}))
					{
						// Emulation stopped
						break;
					}

					obj.clear(), src.close(); // Clear decrypted file and elf object memory
					ppu_initialize(*ovlm, false, file_size);
					ppu_finalize(*ovlm, true);
					break;
				}

				if (ovl_err == elf_error::ok)
				{
					continue;
				}
			}

			ppu_log.notice("Failed to precompile '%s' (prx: %s, ovl: %s): Attempting compilation as executable file", path, prx_err, ovl_err);
			possible_exec_file_paths.push(path, offset, file_size);
			inc_fdone = 0;
		}

		if (restore_mem)
		{
			if (!file_size_limit.fetch_add(restore_mem))
			{
				file_size_limit.notify_all();
			}
		}
	});

	// Join every thread
	workers.join();

	named_thread exec_worker("PPU Exec Worker", [&]
	{
		if (!possible_exec_file_paths)
		{
			return;
		}

#ifdef __APPLE__
		pthread_jit_write_protect_np(false);
#endif
		// Set low priority
		thread_ctrl::scoped_priority low_prio(-1);

		auto slice = possible_exec_file_paths.pop_all();

		auto main_module = std::move(g_fxo->get<main_ppu_module<lv2_obj>>());

		for (; slice; slice.pop_front(), g_progr_fdone++)
		{
			if (Emu.IsStopped())
			{
				continue;
			}

			const auto& [path, _, file_size] = *slice;

			ppu_log.notice("Trying to load as executable: %s", path);

			// Load SELF
			fs::file src{path};

			if (!src)
			{
				ppu_log.error("Failed to open '%s' (%s)", path, fs::g_tls_error);
				continue;
			}

			for (usz i = 0;; i++)
			{
				if (i > decrypt_klics.size())
				{
					src.close();
					break;
				}

				// Some files may fail to decrypt due to the lack of klic
				u128 key = i == decrypt_klics.size() ? u128{} : decrypt_klics[i];

				if (auto result = decrypt_self(src, i == decrypt_klics.size() ? nullptr : reinterpret_cast<const u8*>(&key)))
				{
					src = std::move(result);
					break;
				}
			}

			if (!src && !Emu.klic.empty() && src.open(path))
			{
				src = decrypt_self(src, reinterpret_cast<u8*>(&Emu.klic[0]));

				if (src)
				{
					ppu_log.error("Possible missed KLIC for precompilation of '%s', please report to developers.", path);
				}
			}

			if (!src)
			{
				ppu_log.notice("Failed to decrypt '%s'", path);

				g_progr_ftotal_bits -= file_size;

				continue;
			}

			elf_error exec_err{};

			if (ppu_exec_object obj = src; (exec_err = obj, obj == elf_error::ok))
			{
				while (exec_err == elf_error::ok)
				{
					main_ppu_module<lv2_obj>& _main = g_fxo->get<main_ppu_module<lv2_obj>>();
					_main = {};

					auto current_cache = std::move(g_fxo->get<spu_cache>());

					if (!ppu_load_exec(obj, true, path))
					{
						// Abort
						exec_err = elf_error::header_type;
						break;
					}

					if (std::memcmp(main_module.sha1, _main.sha1, sizeof(_main.sha1)) == 0)
					{
						g_fxo->get<spu_cache>() = std::move(current_cache);
						break;
					}

					if (!_main.analyse(0, _main.elf_entry, _main.seg0_code_end, _main.applied_patches, std::vector<u32>{}, [](){ return Emu.IsStopped(); }))
					{
						g_fxo->get<spu_cache>() = std::move(current_cache);
						break;
					}

					obj.clear(), src.close(); // Clear decrypted file and elf object memory

					_main.name = ' '; // Make ppu_finalize work
					Emu.ConfigurePPUCache();
					ppu_initialize(_main, false, file_size);
					spu_cache::initialize(false);
					ppu_finalize(_main, true);
					_main = {};
					g_fxo->get<spu_cache>() = std::move(current_cache);
					break;
				}

				if (exec_err == elf_error::ok)
				{
					continue;
				}
			}

			ppu_log.notice("Failed to precompile '%s' as executable (%s)", path, exec_err);
		}

		g_fxo->get<main_ppu_module<lv2_obj>>() = std::move(main_module);
		g_fxo->get<spu_cache>().collect_funcs_to_precompile = true;
		Emu.ConfigurePPUCache();
	});

	exec_worker();
}

extern void ppu_initialize()
{
	if (!g_fxo->is_init<main_ppu_module<lv2_obj>>())
	{
		return;
	}

	if (Emu.IsStopped())
	{
		return;
	}

	auto& _main = g_fxo->get<main_ppu_module<lv2_obj>>();

	std::optional<scoped_progress_dialog> progress_dialog(std::in_place, get_localized_string(localized_string_id::PROGRESS_DIALOG_ANALYZING_PPU_EXECUTABLE));

	// Analyse executable
	if (!_main.analyse(0, _main.elf_entry, _main.seg0_code_end, _main.applied_patches, std::vector<u32>{}, [](){ return Emu.IsStopped(); }))
	{
		return;
	}

	// Validate analyser results (not required)
	_main.validate(0);

	*progress_dialog = get_localized_string(localized_string_id::PROGRESS_DIALOG_SCANNING_PPU_MODULES);

	bool compile_main = false;

	// Check main module cache
	if (!_main.segs.empty())
	{
		compile_main = ppu_initialize(_main, true);
	}

	std::vector<ppu_module<lv2_obj>*> module_list;
	module_list.emplace_back(&g_fxo->get<main_ppu_module<lv2_obj>>());

	const std::string firmware_sprx_path = vfs::get("/dev_flash/sys/external/");

	// If empty we have no indication for firmware cache state, check everything
	bool compile_fw = !Emu.IsVsh();

	idm::select<lv2_obj, lv2_prx>([&](u32, lv2_prx& _module)
	{
		if (_module.get_funcs().empty())
		{
			return;
		}

		if (_module.path.starts_with(firmware_sprx_path))
		{
			// Postpone testing
			compile_fw = false;
		}

		module_list.emplace_back(&_module);
	});

	idm::select<lv2_obj, lv2_overlay>([&](u32, lv2_overlay& _module)
	{
		module_list.emplace_back(&_module);
	});

	// Check preloaded libraries cache
	if (!compile_fw)
	{
		for (auto ptr : module_list)
		{
			if (ptr->path.starts_with(firmware_sprx_path))
			{
				compile_fw |= ppu_initialize(*ptr, true);

				// Fixup for compatibility with old savestates
				if (Emu.DeserialManager() && ptr->name == "liblv2.sprx")
				{
					static_cast<lv2_prx*>(ptr)->state = PRX_STATE_STARTED;
					static_cast<lv2_prx*>(ptr)->load_exports();
				}
			}
		}
	}

	std::vector<std::string> dir_queue;

	const std::string mount_point = vfs::get("/dev_flash/");

	bool dev_flash_located = !Emu.GetCat().ends_with('P') && Emu.IsPathInsideDir(Emu.GetBoot(), mount_point) && g_cfg.core.llvm_precompilation;

	if (compile_fw || dev_flash_located)
	{
		if (dev_flash_located)
		{
			const std::string eseibrd = mount_point + "/vsh/module/eseibrd.sprx";

			if (auto prx = ppu_load_prx(ppu_prx_object{decrypt_self(fs::file{eseibrd})}, true, eseibrd, 0))
			{
				// Check if cache exists for this infinitesimally small prx
				dev_flash_located = ppu_initialize(*prx, true);
			}
		}

		const std::string firmware_sprx_path = vfs::get(dev_flash_located ? "/dev_flash/"sv : "/dev_flash/sys/external/"sv);
		dir_queue.emplace_back(firmware_sprx_path);
	}

	// Avoid compilation if main's cache exists or it is a standalone SELF with no PARAM.SFO
	if (compile_main && g_cfg.core.llvm_precompilation && !Emu.GetTitleID().empty() && !Emu.IsChildProcess())
	{
		// Try to add all related directories
		const std::set<std::string> dirs = Emu.GetGameDirs();
		dir_queue.insert(std::end(dir_queue), std::begin(dirs), std::end(dirs));
	}

	progress_dialog.reset();

	ppu_precompile(dir_queue, &module_list, false);

	if (Emu.IsStopped())
	{
		return;
	}

	// Initialize main module cache
	if (!_main.segs.empty())
	{
		ppu_initialize(_main);
	}

	// Initialize preloaded libraries
	for (auto ptr : module_list)
	{
		if (Emu.IsStopped())
		{
			return;
		}

		ppu_initialize(*ptr);
	}
}

bool ppu_initialize(const ppu_module<lv2_obj>& info, bool check_only, u64 file_size)
{
	if (g_cfg.core.ppu_decoder != ppu_decoder_type::llvm)
	{
		if (check_only || vm::base(info.segs[0].addr) != info.segs[0].ptr)
		{
			return false;
		}

		auto& toc_manager = g_fxo->get<ppu_toc_manager>();

		std::lock_guard lock(toc_manager.mutex);

		auto& ppu_toc = toc_manager.toc_map;

		for (const auto& func : info.get_funcs())
		{
			if (func.size && func.blocks.empty())
			{
				ppu_register_function_at(func.addr, func.size);
			}

			for (auto& block : func.blocks)
			{
				if (!block.second)
				{
					continue;
				}

				if (g_fxo->is_init<ppu_far_jumps_t>() && !g_fxo->get<ppu_far_jumps_t>().get_targets(block.first, block.second).empty())
				{
					// Replace the block with ppu_far_jump
					continue;
				}

				ppu_register_function_at(block.first, block.second);
			}

			if (g_cfg.core.ppu_debug && func.size && func.toc != umax && !ppu_get_far_jump(func.addr))
			{
				ppu_toc[func.addr] = func.toc;
				write_to_ptr<ppu_intrp_func_t>(ppu_ptr(func.addr), &ppu_check_toc);
			}
		}

		return false;
	}

	// Link table
	static const std::unordered_map<std::string, u64> s_link_table = []()
	{
		std::unordered_map<std::string, u64> link_table
		{
			{ "sys_game_set_system_sw_version", reinterpret_cast<u64>(ppu_execute_syscall) },
			{ "__trap", reinterpret_cast<u64>(&ppu_trap) },
			{ "__error", reinterpret_cast<u64>(&ppu_error) },
			{ "__check", reinterpret_cast<u64>(&ppu_check) },
			{ "__trace", reinterpret_cast<u64>(&ppu_trace) },
			{ "__syscall", reinterpret_cast<u64>(ppu_execute_syscall) },
			{ "__get_tb", reinterpret_cast<u64>(get_timebased_time) },
			{ "__lwarx", reinterpret_cast<u64>(ppu_lwarx) },
			{ "__ldarx", reinterpret_cast<u64>(ppu_ldarx) },
			{ "__stwcx", reinterpret_cast<u64>(ppu_stwcx) },
			{ "__stdcx", reinterpret_cast<u64>(ppu_stdcx) },
			{ "__dcbz", reinterpret_cast<u64>(+[](u32 addr){ alignas(64) static constexpr u8 z[128]{}; do_cell_atomic_128_store(addr, z); }) },
			{ "__resupdate", reinterpret_cast<u64>(vm::reservation_update) },
			{ "__resinterp", reinterpret_cast<u64>(ppu_reservation_fallback) },
			{ "__escape", reinterpret_cast<u64>(+ppu_escape) },
			{ "__read_maybe_mmio32", reinterpret_cast<u64>(+ppu_read_mmio_aware_u32) },
			{ "__write_maybe_mmio32", reinterpret_cast<u64>(+ppu_write_mmio_aware_u32) },
		};

		for (u64 index = 0; index < 1024; index++)
		{
			if (ppu_get_syscall(index))
			{
				link_table.emplace(fmt::format("%s", ppu_syscall_code(index)), reinterpret_cast<u64>(ppu_execute_syscall));
				link_table.emplace(fmt::format("syscall_%u", index), reinterpret_cast<u64>(ppu_execute_syscall));
			}
		}

		return link_table;
	}();

	// Get cache path for this executable
	std::string cache_path;

	if (!info.cache.empty())
	{
		cache_path = info.cache;
	}
	else
	{
		// New PPU cache location
		cache_path = rpcs3::utils::get_cache_dir(info.path);

		// Add PPU hash and filename
		fmt::append(cache_path, "ppu-%s-%s/", fmt::base57(info.sha1), info.path.substr(info.path.find_last_of('/') + 1));

		if (!fs::create_path(cache_path))
		{
			fmt::throw_exception("Failed to create cache directory: %s (%s)", cache_path, fs::g_tls_error);
		}
	}

#ifdef LLVM_AVAILABLE
	std::optional<scoped_progress_dialog> progress_dialog;

	if (!check_only)
	{
		// Initialize progress dialog
		progress_dialog.emplace(get_localized_string(localized_string_id::PROGRESS_DIALOG_LOADING_PPU_MODULES));
	}

	// Permanently loaded compiled PPU modules (name -> data)
	jit_module& jit_mod = g_fxo->get<jit_module_manager>().get(cache_path + "_" + std::to_string(std::bit_cast<usz>(info.segs[0].ptr)));

	// Compiler instance (deferred initialization)
	std::vector<std::shared_ptr<jit_compiler>>& jits = jit_mod.pjit;

	// Split module into fragments <= 1 MiB
	usz fpos = 0;

	// Modules counted so far
	usz module_counter = 0;

	// Difference between function name and current location
	const u32 reloc = info.is_relocatable ? ::at32(info.segs, 0).addr : 0;

	// Info sent to threads
	std::vector<std::pair<std::string, ppu_module<lv2_obj>>> workload;

	// Info to load to main JIT instance (true - compiled)
	std::vector<std::pair<std::string, bool>> link_workload;

	// Sync variable to acquire workloads
	atomic_t<u32> work_cv = 0;

	bool compiled_new = false;

	bool has_mfvscr = false;

	const bool is_being_used_in_emulation = vm::base(info.segs[0].addr) == info.segs[0].ptr;

	const cpu_thread* cpu = cpu_thread::get_current();

	for (auto& func : info.get_funcs())
	{
		if (func.size == 0)
		{
			continue;
		}

		for (const auto [addr, size] : func)
		{
			if (size == 0)
			{
				continue;
			}

			auto i_ptr = ensure(info.get_ptr<u32>(addr));

			for (u32 i = addr; i < addr + size; i += 4, i_ptr++)
			{
				if (g_ppu_itype.decode(*i_ptr) == ppu_itype::MFVSCR)
				{
					ppu_log.warning("MFVSCR found");
					has_mfvscr = true;
					break;
				}
			}

			if (has_mfvscr)
			{
				break;
			}
		}

		if (has_mfvscr)
		{
			break;
		}
	}

	// Limit how many modules are per JIt instance
	// Advantage to lower the limit:
	// 1. Lowering contoniues memory requirements for allocations
	// Its disadvantage:
	// 1. B instruction can wander up to 16MB relatively to its range,
	// each additional split of JIT instance results in a downgraded version of around (100% / N-1th) - (100% / Nth) percent of instructions
	// where N is the total amunt of JIT instances
	// Subject to change
	constexpr u32 c_moudles_per_jit = 100;

	std::shared_ptr<std::pair<u32, u32>> local_jit_bounds = std::make_shared<std::pair<u32, u32>>(u32{umax}, 0);

	const auto shared_runtime = make_shared<jit_runtime>();
	const auto shared_map = make_shared<std::unordered_map<u32, u64>>();
	const auto full_sample = make_shared<u64>(0);
	const auto shared_mtx = make_shared<shared_mutex>();

	auto symbols_cement = [runtime = shared_runtime, reloc, seg0 = info.segs[0].addr, bound = info.segs[0].addr + info.segs[0].size - reloc, func_map = shared_map, shared_mtx, full_sample](const std::string& name) -> u64
	{
		u32 func_addr = umax;

		if (name.starts_with("__0x"))
		{
			u32 addr = umax;
			auto res = std::from_chars(name.c_str() + 4, name.c_str() + name.size(), addr, 16);

			if (res.ec == std::errc() && res.ptr == name.c_str() + name.size() && addr < bound)
			{
				func_addr = addr + reloc;
			}
		}

		if (func_addr == umax)
		{
			return {};
		}

		reader_lock rlock(*shared_mtx);

		if (auto it = func_map->find(func_addr); it != func_map->end())
		{
			return it->second;
		}

		rlock.upgrade();

		u64& code_ptr = (*func_map)[func_addr];

		if (code_ptr)
		{
			return +code_ptr;
		}

		[[maybe_unused]] constexpr auto abs_diff = [](u64 a, u64 b) { return a <= b ? b - a : a - b; };

		[[maybe_unused]] auto write_le = [](u8*& code, auto value)
		{
			write_to_ptr<le_t<std::remove_cvref_t<decltype(value)>>>(code, value);
			code += sizeof(value);
		};

#if defined(ARCH_X64)
		// Try to make the code fit in 16 bytes, may fail and fallback
		if (*full_sample && (*full_sample <= s32{smax} || abs_diff(*full_sample, reinterpret_cast<u64>(jit_runtime::peek(true))) <= s32{smax}))
		{
			u8* code = jit_runtime::alloc(16, 8, true);
			code_ptr = reinterpret_cast<u64>(code);

			// mov edx, func_addr
			*code++ = 0xba;
			write_le(code, func_addr - seg0);

			const u64 diff_for_jump = abs_diff(reinterpret_cast<u64>(code + 5), *full_sample);

			if (diff_for_jump <= s32{smax})
			{
				// jmp (rel32) full_sample
				*code++ = 0xe9;

				write_le(code, static_cast<s32>(*full_sample - reinterpret_cast<u64>(code + 4)));
				return code_ptr;
			}
			else if (*full_sample <= s32{smax})
			{
				// mov eax, full_sample
				*code++ = 0xb8;

				write_le(code, static_cast<s32>(*full_sample));

				// jmp rax
				*code++ = 0xff;
				*code++ = 0xea;
				return code_ptr;
			}
			else // fallback (requiring more than 16 bytes)
			{
				// movabs rax, full_sample
				// *code++ = 0x48;
				// *code++ = 0xb8;

				// write_le(code, *full_sample);

				// // jmp rax
				// *code++ = 0xff;
				// *code++ = 0xea;
				// return code_ptr;
				ppu_log.error("JIT symbol trampoline failed.");
			}
		}
#elif 0
		// Try to make the code fit in 16 bytes, may fail and fallback
		if (*full_sample && abs_diff(*full_sample, reinterpret_cast<u64>(jit_runtime::peek(true) + 3 * 4)) < (128u << 20))
		{
#ifdef __APPLE__
			pthread_jit_write_protect_np(false);
#endif
			u8* code = jit_runtime::alloc(12, 4, true);
			code_ptr = reinterpret_cast<u64>(code);

			union arm_op
			{
				u32 op;
				bf_t<u32, 0, 26> b_target;
				bf_t<u32, 5, 16> mov_imm16;
			};

			const u64 diff_for_jump = abs_diff(reinterpret_cast<u64>(code + 3 * 4), *full_sample);

			if (diff_for_jump < (128u << 20))
			{
				// MOVZ w15, func_addr
				arm_op mov_pcl{0x5280000F};
				mov_pcl.mov_imm16 = func_addr & 0xffff;

				write_le(code, mov_pcl.op);

				// MOVK w15, func_addr >> 16, LSL #16
				arm_op mov_pch{0x72A0000F};
				mov_pch.mov_imm16 = func_addr >> 16;

				write_le(code, mov_pch.op);

				const s64 branch_offset = (*full_sample - reinterpret_cast<u64>(code + 4));

				// B full_sample
				arm_op b_sample{0x14000000};
				b_sample.b_target = static_cast<s32>(branch_offset / 4);

				write_le(code, b_sample.op);
				return code_ptr;
			}
			else // fallback
			{
				ppu_log.error("JIT symbol trampoline failed.");
			}
		}
#endif

		using namespace asmjit;

		usz code_size_until_jump = umax;

		auto func = build_function_asm<u8*(*)(ppu_thread&, u64, u8*, u64, u64, u64)>(name, [&](native_asm& c, auto& /*args*/)
		{
#if defined(ARCH_X64)
			c.mov(x86::edx, func_addr - seg0); // Load PC

			const auto buf_start = reinterpret_cast<const u8*>(c.bufferData());
			const auto buf_end = reinterpret_cast<const u8*>(c.bufferPtr());

			code_size_until_jump = buf_end - buf_start;

			c.add(x86::edx, seg0);
			c.movabs(x86::rax, reinterpret_cast<u64>(&vm::g_exec_addr));
			c.mov(x86::rax, x86::qword_ptr(x86::rax));
			c.mov(x86::dword_ptr(x86::rbp, ::offset32(&ppu_thread::cia)), x86::edx);

			c.mov(x86::rcx, x86::qword_ptr(x86::rax, x86::rdx, 1, 0)); // Load call target
			c.movabs(x86::r12, vm::g_exec_addr_seg_offset);
			c.add(x86::rax, x86::r12);
			c.shr(x86::edx, 1);
			c.mov(x86::edx, x86::word_ptr(x86::rax, x86::edx)); // Load relocation base
			c.shl(x86::edx, 13);
			c.mov(x86::r12d, x86::edx); // Set relocation base
			c.jmp(x86::rcx);
#else
			// Load REG_Base - use absolute jump target to bypass rel jmp range limits
			// X19 contains vm::g_exec_addr
			const arm::GpX exec_addr = a64::x19;

			// X20 contains ppu_thread*
			const arm::GpX ppu_t_base = a64::x20;

			// Load PC
			const arm::GpX pc = a64::x15;
			const arm::GpX cia_addr_reg = a64::x11;

			// Load CIA
			c.mov(pc.w(), func_addr);

			const auto buf_start = reinterpret_cast<const u8*>(c.bufferData());
			const auto buf_end = reinterpret_cast<const u8*>(c.bufferPtr());

			code_size_until_jump = buf_end - buf_start;

			// Load offset value
			c.mov(cia_addr_reg, static_cast<u64>(::offset32(&ppu_thread::cia)));

			// Update CIA
			c.str(pc.w(), arm::Mem(ppu_t_base, cia_addr_reg));

			// Multiply by 2 to index into ptr table
			c.add(pc, pc, pc);

			// Load call target
			const arm::GpX call_target = a64::x13;
			c.ldr(call_target, arm::Mem(exec_addr, pc));

			// Compute REG_Hp
			const arm::GpX reg_hp = a64::x21;
			c.mov(reg_hp, Imm(vm::g_exec_addr_seg_offset));
			c.add(reg_hp, reg_hp, pc, arm::Shift(arm::ShiftOp::kLSR, 2));
			c.ldrh(reg_hp.w(), arm::Mem(exec_addr, reg_hp));
			c.lsl(reg_hp.w(), reg_hp.w(), 13);

			// Execute LLE call
			c.br(call_target);
#endif
		}, runtime.get(), true);

		// Full sample may exist already, but is very far away
		// So in this case, a new sample is written
		ensure(code_size_until_jump != umax);
		*full_sample = reinterpret_cast<u64>(func) + code_size_until_jump;

		code_ptr = reinterpret_cast<u64>(func);
		return code_ptr;
	};

	if (has_mfvscr && g_cfg.core.ppu_set_sat_bit)
	{
		info.attr += ppu_attr::has_mfvscr;
	}

	while (!jit_mod.init && fpos < info.get_funcs().size())
	{
		// Copy module information
		ppu_module<lv2_obj> part;
		part.copy_part(info);

		// Overall block size in bytes
		usz bsize = 0;
		usz bcount = 0;

		while (fpos < info.get_funcs().size())
		{
			auto& func = info.get_funcs()[fpos];

			if (!func.size)
			{
				fpos++;
				continue;
			}

			if (bsize + func.size > 100 * 1024 && bsize)
			{
				if (bcount >= 1000)
				{
					break;
				}
			}

			if (g_fxo->is_init<ppu_far_jumps_t>())
			{
				auto targets = g_fxo->get<ppu_far_jumps_t>().get_targets(func.addr, func.size);

				for (auto [source, target] : targets)
				{
					auto far_jump = ensure(g_fxo->get<ppu_far_jumps_t>().gen_jump(source));

					if (source == func.addr)
					{
						(*shared_map)[func.addr] = reinterpret_cast<u64>(far_jump);
					}

					ppu_register_function_at(source, 4, far_jump);
				}

				if (!targets.empty())
				{
					// Replace the function with ppu_far_jump
					fpos++;
					part.excluded_funcs.emplace_back(func.addr);
					continue;
				}
			}

			local_jit_bounds->first = std::min<u32>(local_jit_bounds->first, func.addr);
			local_jit_bounds->second = std::max<u32>(local_jit_bounds->second, func.addr + func.size);

			part.local_bounds.first = std::min<u32>(part.local_bounds.first, func.addr);
			part.local_bounds.second = std::max<u32>(part.local_bounds.second, func.addr + func.size);

			bsize += func.size;

			fpos++;
			bcount++;
		}

		// Compute module hash to generate (hopefully) unique object name
		std::string obj_name;
		{
			sha1_context ctx;
			u8 output[20];
			sha1_starts(&ctx);

			int has_dcbz = !!g_cfg.core.accurate_cache_line_stores;

			for (const auto& func : part.get_funcs())
			{
				if (func.size == 0)
				{
					continue;
				}

				if (std::count(part.excluded_funcs.begin(), part.excluded_funcs.end(), func.addr))
				{
					continue;
				}

				const be_t<u32> addr = func.addr - reloc;
				const be_t<u32> size = func.size;
				sha1_update(&ctx, reinterpret_cast<const u8*>(&addr), sizeof(addr));
				sha1_update(&ctx, reinterpret_cast<const u8*>(&size), sizeof(size));

				for (const auto block : func)
				{
					if (block.second == 0 || reloc)
					{
						continue;
					}

					// Find relevant relocations
					auto low = std::lower_bound(part.relocs.cbegin(), part.relocs.cend(), block.first);
					auto high = std::lower_bound(low, part.relocs.cend(), block.first + block.second);
					auto addr = block.first;

					for (; low != high; ++low)
					{
						// Aligned relocation address
						const u32 roff = low->addr & ~3;

						if (roff > addr)
						{
							// Hash from addr to the beginning of the relocation
							sha1_update(&ctx, ensure(info.get_ptr<const u8>(addr)), roff - addr);
						}

						// Hash relocation type instead
						const be_t<u32> type = low->type;
						sha1_update(&ctx, reinterpret_cast<const u8*>(&type), sizeof(type));

						// Set the next addr
						addr = roff + 4;
					}

					if (has_dcbz == 1)
					{
						auto i_ptr = ensure(info.get_ptr<u32>(addr));

						for (u32 i = addr, end = block.second + block.first - 1; i <= end; i += 4, i_ptr++)
						{
							if (g_ppu_itype.decode(*i_ptr) == ppu_itype::DCBZ)
							{
								has_dcbz = 2;
								break;
							}
						}
					}

					// Hash from addr to the end of the block
					sha1_update(&ctx, ensure(info.get_ptr<const u8>(addr)), block.second - (addr - block.first));
				}

				if (reloc)
				{
					continue;
				}

				if (has_dcbz == 1)
				{
					auto i_ptr = ensure(info.get_ptr<u32>(func.addr));

					for (u32 i = func.addr, end = func.addr + func.size - 1; i <= end; i += 4, i_ptr++)
					{
						if (g_ppu_itype.decode(*i_ptr) == ppu_itype::DCBZ)
						{
							has_dcbz = 2;
							break;
						}
					}
				}

				sha1_update(&ctx, ensure(info.get_ptr<const u8>(func.addr)), func.size);
			}

			if (fpos >= info.get_funcs().size() || module_counter % c_moudles_per_jit == c_moudles_per_jit - 1)
			{
				// Hash the entire function grouped addresses for the integrity of the symbol resolver function
				// Potentially occuring during patches
				// Avoid doing it for files with a single module such as most PRX

				std::vector<be_t<u32>> addrs;

				constexpr auto compare = [](const ppu_function& a, u32 addr) { return a.addr < addr; };

				const auto start = std::lower_bound(info.funcs.begin(), info.funcs.end(), local_jit_bounds->first, compare);

				std::span<const ppu_function> span_range{ start, std::lower_bound(start, info.funcs.end(), local_jit_bounds->second, compare) };

				for (const ppu_function& func : span_range)
				{
					if (func.size == 0)
					{
						continue;
					}

					if (g_fxo->is_init<ppu_far_jumps_t>() && !g_fxo->get<ppu_far_jumps_t>().get_targets(func.addr, func.size).empty())
					{
						// Filter out functions with patches
						part.excluded_funcs.emplace_back(func.addr);
						continue;
					}

					addrs.emplace_back(func.addr - reloc);
				}

				// Hash its size too
				addrs.emplace_back(::size32(addrs));

				if (module_counter != 0)
				{
					sha1_update(&ctx, reinterpret_cast<const u8*>(addrs.data()), addrs.size() * sizeof(be_t<u32>));
				}

				part.jit_bounds = std::move(local_jit_bounds);
				local_jit_bounds = std::make_shared<std::pair<u32, u32>>(u32{umax}, 0);
			}

			if (false)
			{
				const be_t<u64> forced_upd = 3;
				sha1_update(&ctx, reinterpret_cast<const u8*>(&forced_upd), sizeof(forced_upd));
			}

			sha1_finish(&ctx, output);

			// Settings: should be populated by settings which affect codegen (TODO)
			enum class ppu_settings : u32
			{
				platform_bit,
				accurate_dfma,
				fixup_vnan,
				fixup_nj_denormals,
				accurate_cache_line_stores,
				reservations_128_byte,
				greedy_mode,
				accurate_sat,
				accurate_fpcc,
				accurate_vnan,
				accurate_nj_mode,
				contains_symbol_resolver,

				__bitset_enum_max
			};

			be_t<bs_t<ppu_settings>> settings{};

#if !defined(_WIN32) && !defined(__APPLE__)
			settings += ppu_settings::platform_bit;
#endif
			if (g_cfg.core.use_accurate_dfma)
				settings += ppu_settings::accurate_dfma;
			if (g_cfg.core.ppu_fix_vnan)
				settings += ppu_settings::fixup_vnan;
			if (g_cfg.core.ppu_llvm_nj_fixup)
				settings += ppu_settings::fixup_nj_denormals;
			if (has_dcbz == 2)
				settings += ppu_settings::accurate_cache_line_stores;
			if (g_cfg.core.ppu_128_reservations_loop_max_length)
				settings += ppu_settings::reservations_128_byte;
			if (g_cfg.core.ppu_llvm_greedy_mode)
				settings += ppu_settings::greedy_mode;
			if (has_mfvscr && g_cfg.core.ppu_set_sat_bit)
				settings += ppu_settings::accurate_sat;
			if (g_cfg.core.ppu_set_fpcc)
				settings += ppu_settings::accurate_fpcc, fmt::throw_exception("FPCC Not implemented");
			if (g_cfg.core.ppu_set_vnan)
				settings += ppu_settings::accurate_vnan, settings -= ppu_settings::fixup_vnan, fmt::throw_exception("VNAN Not implemented");
			if (g_cfg.core.ppu_use_nj_bit)
				settings += ppu_settings::accurate_nj_mode, settings -= ppu_settings::fixup_nj_denormals, fmt::throw_exception("NJ Not implemented");
			if (fpos >= info.get_funcs().size() || module_counter % c_moudles_per_jit == c_moudles_per_jit - 1)
				settings += ppu_settings::contains_symbol_resolver; // Avoid invalidating all modules for this purpose

			// Write version, hash, CPU, settings
			fmt::append(obj_name, "v7-kusa-%s-%s-%s.obj", fmt::base57(output, 16), fmt::base57(settings), jit_compiler::cpu(g_cfg.core.llvm_cpu));
		}

		if (cpu ? cpu->state.all_of(cpu_flag::exit) : Emu.IsStopped())
		{
			break;
		}

		module_counter++;

		if (!check_only)
		{
			link_workload.emplace_back(obj_name, false);
		}

		// Check object file
		if (jit_compiler::check(cache_path + obj_name))
		{
			if (!is_being_used_in_emulation && !check_only)
			{
				ppu_log.success("LLVM: Module exists: %s", obj_name);
				link_workload.pop_back();
			}

			continue;
		}

		if (check_only)
		{
			return true;
		}

		// Remember, used in ppu_initialize(void)
		compiled_new = true;

		// Adjust information (is_compiled)
		link_workload.back().second = true;

		// Fill workload list for compilation
		workload.emplace_back(std::move(obj_name), std::move(part));
	}

	if (check_only)
	{
		return false;
	}

	if (g_progr_ftotal_bits && file_size)
	{
		g_progr_fknown_bits += file_size;
	}

	// Create worker threads for compilation
	if (!workload.empty())
	{
		// Update progress dialog
		g_progr_ptotal += ::size32(workload);

		*progress_dialog = get_localized_string(localized_string_id::PROGRESS_DIALOG_COMPILING_PPU_MODULES);

		const u32 thread_count = std::min(::size32(workload), rpcs3::utils::get_max_threads());

		struct thread_index_allocator
		{
			atomic_t<u64> index = 0;
		};

		struct thread_op
		{
			atomic_t<u32>& work_cv;
			std::vector<std::pair<std::string, ppu_module<lv2_obj>>>& workload;
			const ppu_module<lv2_obj>& main_module;
			const std::string& cache_path;
			const cpu_thread* cpu;

			std::unique_lock<decltype(jit_core_allocator::sem)> core_lock;

			thread_op(atomic_t<u32>& work_cv, std::vector<std::pair<std::string, ppu_module<lv2_obj>>>& workload
				, const cpu_thread* cpu, const ppu_module<lv2_obj>& main_module, const std::string& cache_path, decltype(jit_core_allocator::sem)& sem) noexcept

				: work_cv(work_cv)
				, workload(workload)
				, main_module(main_module)
				, cache_path(cache_path)
				, cpu(cpu)
			{
				// Save mutex
				core_lock = std::unique_lock{sem, std::defer_lock};
			}

			thread_op(const thread_op& other) noexcept
				: work_cv(other.work_cv)
				, workload(other.workload)
				, main_module(other.main_module)
				, cache_path(other.cache_path)
				, cpu(other.cpu)
			{
				if (auto mtx = other.core_lock.mutex())
				{
					// Save mutex
					core_lock = std::unique_lock{*mtx, std::defer_lock};
				}
			}

			thread_op(thread_op&& other) noexcept = default;

			void operator()()
			{
				// Set low priority
				thread_ctrl::scoped_priority low_prio(-1);

	#ifdef __APPLE__
				pthread_jit_write_protect_np(false);
	#endif
				for (u32 i = work_cv++; i < workload.size(); i = work_cv++, g_progr_pdone++)
				{
					if (cpu ? cpu->state.all_of(cpu_flag::exit) : Emu.IsStopped())
					{
						continue;
					}

					// Keep allocating workload
					const auto& [obj_name, part] = std::as_const(workload)[i];

					std::shared_lock rlock(g_fxo->get<jit_core_allocator>().shared_mtx, std::defer_lock);
					std::unique_lock lock(g_fxo->get<jit_core_allocator>().shared_mtx, std::defer_lock);

					if (false && part.jit_bounds && part.parent->funcs.size() >= 0x8000)
					{
						// Make a large symbol-resolving function compile alone because it has massive memory requirements
						lock.lock();
					}
					else
					{
						rlock.lock();
					}

					ppu_log.warning("LLVM: Compiling module %s%s", cache_path, obj_name);

					{
						// Use another JIT instance
						jit_compiler jit2({}, g_cfg.core.llvm_cpu, 0x1);
						ppu_initialize2(jit2, part, cache_path, obj_name);
					}

					ppu_log.success("LLVM: Compiled module %s", obj_name);
				}

				core_lock.unlock();
			}
		};

		// Prevent watchdog thread from terminating
		g_watchdog_hold_ctr++;

		named_thread_group threads(fmt::format("PPUW.%u.", ++g_fxo->get<thread_index_allocator>().index), thread_count
			, thread_op(work_cv, workload, cpu, info, cache_path, g_fxo->get<jit_core_allocator>().sem)
			, [&](u32 /*thread_index*/, thread_op& op)
		{
			// Allocate "core"
			op.core_lock.lock();

			// Second check before creating another thread
			return work_cv < workload.size() && (cpu ? !cpu->state.all_of(cpu_flag::exit) : !Emu.IsStopped());
		});

		threads.join();

		g_watchdog_hold_ctr--;
	}

	// Initialize compiler instance
	while (jits.size() < utils::aligned_div<u64>(module_counter, c_moudles_per_jit) && is_being_used_in_emulation)
	{
		jits.emplace_back(std::make_shared<jit_compiler>(s_link_table, g_cfg.core.llvm_cpu, 0, symbols_cement));

		for (const auto& [addr, func] : *shared_map)
		{
			jits.back()->update_global_mapping(fmt::format("__0x%x", addr - reloc), func);
		}
	}

	if (jit_mod.symbol_resolvers.empty() && is_being_used_in_emulation)
	{
		jit_mod.symbol_resolvers.resize(jits.size());
	}

	bool failed_to_load = false;
	{
		if (!is_being_used_in_emulation || (cpu ? cpu->state.all_of(cpu_flag::exit) : Emu.IsStopped()))
		{
			return compiled_new;
		}

		*progress_dialog = get_localized_string(localized_string_id::PROGRESS_DIALOG_LINKING_PPU_MODULES);

		// Because linking is faster than compiling, consider each module linkages as a single module compilation in time
		const bool divide_by_twenty = !workload.empty();
		const usz increment_link_count_at = (divide_by_twenty ? 20 : 1);

		g_progr_ptotal += static_cast<u32>(utils::aligned_div<u64>(link_workload.size(), increment_link_count_at));

		usz mod_index = umax;

		for (const auto& [obj_name, is_compiled] : link_workload)
		{
			mod_index++;

			if (cpu ? cpu->state.all_of(cpu_flag::exit) : Emu.IsStopped())
			{
				break;
			}

			if (!failed_to_load && !jits[mod_index / c_moudles_per_jit]->add(cache_path + obj_name))
			{
				ppu_log.error("LLVM: Failed to load module %s", obj_name);
				failed_to_load = true;
			}

			if (mod_index % increment_link_count_at == (link_workload.size() - 1) % increment_link_count_at)
			{
				// Incremenet 'pdone' Nth times where N is link workload size ceil-divided by increment_link_count_at
				g_progr_pdone++;
			}

			if (failed_to_load)
			{
				continue;
			}

			if (!is_compiled)
			{
				ppu_log.success("LLVM: Loaded module %s", obj_name);
			}
		}
	}

	if (failed_to_load || !is_being_used_in_emulation || (cpu ? cpu->state.all_of(cpu_flag::exit) : Emu.IsStopped()))
	{
		return compiled_new;
	}

	// Jit can be null if the loop doesn't ever enter.
#ifdef __APPLE__
	pthread_jit_write_protect_np(false);
#endif
	// Try to patch all single and unregistered BLRs with the same function (TODO: Maybe generalize it into PIC code detection and patching)
	ppu_intrp_func_t BLR_func = nullptr;

	const bool showing_only_apply_stage = !g_progr_text.operator bool() && !g_progr_ptotal && !g_progr_ftotal && g_progr_ptotal.compare_and_swap_test(0, 1);

	progress_dialog = get_localized_string(localized_string_id::PROGRESS_DIALOG_APPLYING_PPU_CODE);

	if (jits.empty())
	{
		// No functions - nothing to do
		ensure(info.get_funcs().empty());
		return compiled_new;
	}

	const bool is_first = !jit_mod.init;

	if (is_first)
	{
		for (auto& jit : jits)
		{
			jit->fin();
		}
	}

#ifdef __APPLE__
	// Symbol resolver is in JIT mem, so we must enable execution
	pthread_jit_write_protect_np(true);
#endif
	{
		usz index = umax;

		for (auto& sim : jit_mod.symbol_resolvers)
		{
			index++;

			sim = ensure(!is_first ? sim : reinterpret_cast<void(*)(u8*, u64)>(jits[index]->get("__resolve_symbols")));
			sim(vm::g_exec_addr, info.segs[0].addr);
		}
	}

#ifdef __APPLE__
	// Symbol resolver is in JIT mem, so we must enable execution
	pthread_jit_write_protect_np(false);
#endif

	// Find a BLR-only function in order to copy it to all BLRs (some games need it)
	for (const auto& func : info.get_funcs())
	{
		if (func.size == 4 && *info.get_ptr<u32>(func.addr) == ppu_instructions::BLR())
		{
			BLR_func = ppu_read(func.addr);
			break;
		}
	}

	if (is_first)
	{
		jit_mod.init = true;
	}

	if (BLR_func)
	{
		auto inst_ptr = info.get_ptr<u32>(info.segs[0].addr);

		for (u32 addr = info.segs[0].addr; addr < info.segs[0].addr + info.segs[0].size; addr += 4, inst_ptr++)
		{
			if (*inst_ptr == ppu_instructions::BLR() && reinterpret_cast<uptr>(ppu_read(addr)) == reinterpret_cast<uptr>(ppu_recompiler_fallback_ghc))
			{
				write_to_ptr<ppu_intrp_func_t>(ppu_ptr(addr), BLR_func);
			}
		}
	}

	if (showing_only_apply_stage)
	{
		// Done
		g_progr_pdone++;
	}

	return compiled_new;
#else
	fmt::throw_exception("LLVM is not available in this build.");
#endif
}

static void ppu_initialize2(jit_compiler& jit, const ppu_module<lv2_obj>& module_part, const std::string& cache_path, const std::string& obj_name)
{
#ifdef LLVM_AVAILABLE
	using namespace llvm;

	// Create LLVM module
	std::unique_ptr<Module> _module = std::make_unique<Module>(obj_name, jit.get_context());

	// Initialize target
#if LLVM_VERSION_MAJOR >= 21 && (LLVM_VERSION_MINOR >= 1 || LLVM_VERSION_MAJOR >= 22)
	_module->setTargetTriple(Triple(jit_compiler::triple1()));
#else
	_module->setTargetTriple(jit_compiler::triple1());
#endif
	_module->setDataLayout(jit.get_engine().getTargetMachine()->createDataLayout());

	// Initialize translator
	PPUTranslator translator(jit.get_context(), _module.get(), module_part, jit.get_engine());

	// Define some types
	const auto _func = FunctionType::get(translator.get_type<void>(), {
		translator.get_type<u8*>(), // Exec base
		translator.get_type<u8*>(), // PPU context
		translator.get_type<u64>(), // Segment address (for PRX)
		translator.get_type<u8*>(), // Memory base
		translator.get_type<u64>(), // r0
		translator.get_type<u64>(), // r1
		translator.get_type<u64>(), // r2
		}, false);

	// Difference between function name and current location
	const u32 reloc = module_part.is_relocatable ? ::at32(module_part.segs, 0).addr : 0;

	// Initialize function list
	for (const auto& func : module_part.get_funcs())
	{
		if (func.size)
		{
			if (std::count(module_part.excluded_funcs.begin(), module_part.excluded_funcs.end(), func.addr))
			{
				continue;
			}

			const auto f = cast<Function>(_module->getOrInsertFunction(fmt::format("__0x%x", func.addr - reloc), _func).getCallee());
			f->setCallingConv(CallingConv::GHC);
			f->addParamAttr(1, llvm::Attribute::NoAlias);
			f->addFnAttr(Attribute::NoUnwind);
		}
	}

	{
		if (g_cfg.core.ppu_debug)
		{
			translator.build_interpreter();
		}

		// Create the analysis managers.
		// These must be declared in this order so that they are destroyed in the
		// correct order due to inter-analysis-manager references.
		LoopAnalysisManager lam;
		FunctionAnalysisManager fam;
		CGSCCAnalysisManager cgam;
		ModuleAnalysisManager mam;

		// Create the new pass manager builder.
		// Take a look at the PassBuilder constructor parameters for more
		// customization, e.g. specifying a TargetMachine or various debugging
		// options.
		PassBuilder pb;

		// Register all the basic analyses with the managers.
		pb.registerModuleAnalyses(mam);
		pb.registerCGSCCAnalyses(cgam);
		pb.registerFunctionAnalyses(fam);
		pb.registerLoopAnalyses(lam);
		pb.crossRegisterProxies(lam, fam, cgam, mam);

		FunctionPassManager fpm;
		// Basic optimizations
		fpm.addPass(EarlyCSEPass());

		u32 guest_code_size = 0;
		u32 min_addr = umax;
		u32 max_addr = 0;
		u32 num_func = 0;

		// Translate functions
		// Start with the lowest bound of the module, function list is sorted
		for (const auto& mod_func : module_part.get_funcs())
		{
			if (Emu.IsStopped())
			{
				ppu_log.success("LLVM: Translation cancelled");
				return;
			}

			if (mod_func.size)
			{
				if (std::count(module_part.excluded_funcs.begin(), module_part.excluded_funcs.end(), mod_func.addr))
				{
					continue;
				}

				num_func++;
				guest_code_size += mod_func.size;
				max_addr = std::max<u32>(max_addr, mod_func.addr + mod_func.size);
				min_addr = std::min<u32>(min_addr, mod_func.addr);

				// Translate
				if ([[maybe_unused]] const auto func = translator.Translate(mod_func))
				{
#ifdef ARCH_X64 // TODO
					// Run optimization passes
					fpm.run(*func, fam);
#endif // ARCH_X64
				}
				else
				{
					Emu.Pause();
					return;
				}
			}
		}

		// Run this only in one module for all functions compiled
		if (module_part.jit_bounds)
		{
			if ([[maybe_unused]] const auto func = translator.GetSymbolResolver(module_part))
			{
#ifdef ARCH_X64 // TODO
				// Run optimization passes
				fpm.run(*func, fam);
#endif // ARCH_X64
			}
			else
			{
				Emu.Pause();
				return;
			}
		}

		//legacy::PassManager mpm;

		// Remove unused functions, structs, global variables, etc
		//mpm.add(createStripDeadPrototypesPass());
		//mpm.add(createFunctionInliningPass());
		//mpm.add(createDeadInstEliminationPass());
		//mpm.run(*module);

		std::string result;
		raw_string_ostream out(result);

		if (g_cfg.core.llvm_logs)
		{
			out << *_module; // print IR
			fs::write_file(cache_path + obj_name + ".log", fs::rewrite, out.str());
			result.clear();
		}

		if (verifyModule(*_module, &out))
		{
			out.flush();
			ppu_log.error("LLVM: Verification failed for %s:\n%s", obj_name, result);
			Emu.CallFromMainThread([]{ Emu.GracefulShutdown(false, true); });
			return;
		}

		ppu_log.notice("LLVM: %zu functions generated (code_size=0x%x, num_func=%d, max_addr(-)min_addr=0x%x)", _module->getFunctionList().size(), guest_code_size, num_func, max_addr - min_addr);
	}

	// Load or compile module
	jit.add(std::move(_module), cache_path);
#endif // LLVM_AVAILABLE
}
