#pragma once

#include "PPUOpcodes.h"

class ppu_thread;

using ppu_intrp_func_t = void(*)(ppu_thread& ppu_, ppu_opcode_t op, be_t<u32>* this_op, struct ppu_intrp_func* next_fn);

struct ppu_intrp_func
{
	ppu_intrp_func_t fn;
#if defined(__EMSCRIPTEN__) || defined(__wasm__)
	// wasm32 function pointers are 4 bytes (table indices), so sizeof
	// would naturally be 4. But the interpreter cache uses `cia * 2`
	// as the stride (one slot per 4-byte PPU instruction, 8 bytes per
	// slot to match x86-64). exec_task chains via `fn + 1`, which on
	// 32-bit advances by sizeof(ppu_intrp_func) = 4 — landing in the
	// middle of the current slot instead of at the next instruction.
	// Pad to 8 bytes so the layout matches the cache stride.
	u32 _wasm_pad;
#endif
};

template <typename IT>
struct ppu_interpreter_t;

namespace asmjit
{
	struct ppu_builder;
}

struct ppu_interpreter_rt_base
{
protected:
	std::unique_ptr<ppu_interpreter_t<ppu_intrp_func_t>> ptrs;

	ppu_interpreter_rt_base() noexcept;

	ppu_interpreter_rt_base(const ppu_interpreter_rt_base&) = delete;

	ppu_interpreter_rt_base& operator=(const ppu_interpreter_rt_base&) = delete;

	virtual ~ppu_interpreter_rt_base();
};

struct ppu_interpreter_rt : ppu_interpreter_rt_base
{
	ppu_interpreter_rt() noexcept;

	ppu_intrp_func_t decode(u32 op) const noexcept;

private:
	ppu_decoder<ppu_interpreter_t<ppu_intrp_func_t>, ppu_intrp_func_t> table;
};
