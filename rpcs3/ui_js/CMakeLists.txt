cmake_minimum_required(VERSION 3.18)

# rpcs3 WebGPU/Emscripten target — POC scaffold.
#
# Builds a standalone Rpcs3{Variant}.js / Rpcs3{Variant}.wasm pair that the
# React shell under js/rpcs3_browser loads at runtime. This target is
# DELIBERATELY decoupled from the main rpcs3 emucore static lib (which
# pulls in LLVM, Vulkan, Qt, hidapi, libusb, etc. — none of which build
# cleanly under emcc today). When the emucore is portable enough, link it
# in here and replace the printf stubs in Rpcs3VmJs.cpp with real Emu.*
# calls.
#
# Driven by ../../build_wasm.sh which patches OUTPUT_NAME / EXPORT_NAME /
# INITIAL_MEMORY in place for each memory variant.

project(Rpcs3JS)

set(CMAKE_CXX_STANDARD 23)

option(ENABLE_WEBGPU "Enable WebGPU graphics backend" ON)

set(RPCS3_JS_SOURCES
    Main.cpp
    Rpcs3VmJs.cpp
    Rpcs3VmJs.h
    InputProviderEmscripten.cpp
    InputProviderEmscripten.h
    JsDiscBridge.cpp
    JsDiscBridge.h
)
if(ENABLE_WEBGPU)
    list(APPEND RPCS3_JS_SOURCES GSH_WebGPUJs.cpp GSH_WebGPUJs.h)
endif()

# WebGPUGSRender requires emucore for rsx::thread / GSRender / display_flip_info_t
if(HAS_EMUCORE)
    list(APPEND RPCS3_JS_SOURCES WebGPUGSRender.cpp WebGPUGSRender.h)
endif()

# Phase 3 only: the fs::file_base subclass needs emucore headers
# (Utilities/File.h pulls util/atomic.hpp etc.). Skipped in the
# standalone scaffold so JsDiscBridge can still be smoke-tested
# without linking the full emucore. Flip HAS_EMUCORE=ON when linking
# the emucore static lib (Phase 3).
option(HAS_EMUCORE "Link the rpcs3 emucore into the WASM target" OFF)
if(HAS_EMUCORE)
    list(APPEND RPCS3_JS_SOURCES JsDiscFile.cpp JsDiscFile.h EmscriptenStubs.cpp)
endif()

add_executable(Rpcs3 ${RPCS3_JS_SOURCES})
if(HAS_EMUCORE)
    target_compile_definitions(Rpcs3 PRIVATE HAS_EMUCORE)
    target_include_directories(Rpcs3 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../.."
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/ffmpeg/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/_emscripten_stubs/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/cubeb/cubeb/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/exports"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/protobuf/protobuf/third_party/utf8_range"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/pugixml/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/glslang/glslang/SPIRV/.."
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/glslang/glslang/glslang/.."
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/yaml-cpp/yaml-cpp/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/zstd/zstd/build/cmake/../../lib"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/asmjit/asmjit/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/stblib/stb"
    )

    target_link_libraries(Rpcs3 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/rpcs3/Emu/librpcs3_emu.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/3rdparty/pugixml/libpugixml.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/3rdparty/yaml-cpp/yaml-cpp/libyaml-cpp.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/3rdparty/zstd/zstd/build/cmake/lib/libzstd.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/3rdparty/glslang/glslang/glslang/libglslang.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/3rdparty/glslang/glslang/SPIRV/libSPIRV.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/3rdparty/asmjit/libasmjit.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/3rdparty/protobuf/protobuf/libprotobuf.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/3rdparty/protobuf/protobuf/third_party/utf8_range/libutf8_validity.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/3rdparty/cubeb/cubeb/libcubeb.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/3rdparty/libpng/libpng/libpng16.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/strings/libabsl_cord.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/strings/libabsl_string_view.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/strings/libabsl_cord_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/strings/libabsl_cordz_info.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/strings/libabsl_strings.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/strings/libabsl_str_format_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/strings/libabsl_strings_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/strings/libabsl_cordz_handle.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/strings/libabsl_cordz_functions.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/flags/libabsl_flags_commandlineflag.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/flags/libabsl_flags_private_handle_accessor.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/flags/libabsl_flags_program_name.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/flags/libabsl_flags_config.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/flags/libabsl_flags_commandlineflag_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/flags/libabsl_flags_marshalling.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/flags/libabsl_flags_reflection.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/flags/libabsl_flags_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/synchronization/libabsl_kernel_timeout_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/synchronization/libabsl_graphcycles_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/synchronization/libabsl_synchronization.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/hash/libabsl_city.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/hash/libabsl_hash.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/hash/libabsl_low_level_hash.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/debugging/libabsl_utf8_for_code_point.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/debugging/libabsl_decode_rust_punycode.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/debugging/libabsl_debugging_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/debugging/libabsl_symbolize.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/debugging/libabsl_stacktrace.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/debugging/libabsl_demangle_rust.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/debugging/libabsl_demangle_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/debugging/libabsl_leak_check.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/debugging/libabsl_examine_stack.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/crc/libabsl_crc_cord_state.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/crc/libabsl_crc32c.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/crc/libabsl_crc_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/crc/libabsl_crc_cpu_detect.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/status/libabsl_status.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/status/libabsl_statusor.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/time/libabsl_time_zone.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/time/libabsl_time.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/time/libabsl_civil_time.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/container/libabsl_raw_hash_set.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/container/libabsl_hashtablez_sampler.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/numeric/libabsl_int128.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/profiling/libabsl_exponential_biased.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_die_if_null.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_internal_conditions.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_internal_nullguard.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_internal_log_sink_set.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_internal_format.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_internal_fnmatch.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_internal_globals.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_sink.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_internal_check_op.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_internal_structured_proto.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_internal_message.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_initialize.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_globals.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_log_internal_proto.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/log/libabsl_vlog_config_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/random/libabsl_random_distributions.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/random/libabsl_random_seed_sequences.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/random/libabsl_random_internal_platform.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/random/libabsl_random_internal_entropy_pool.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/random/libabsl_random_internal_seed_material.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/random/libabsl_random_internal_randen_slow.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/random/libabsl_random_internal_randen_hwaes_impl.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/random/libabsl_random_internal_randen_hwaes.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/random/libabsl_random_internal_randen.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/random/libabsl_random_seed_gen_exception.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/base/libabsl_spinlock_wait.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/base/libabsl_log_severity.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/base/libabsl_tracing_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/base/libabsl_raw_logging_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/base/libabsl_base.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/base/libabsl_throw_delegate.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/base/libabsl_malloc_internal.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build_cmake/build/root-wasm/_deps/absl-build/absl/base/libabsl_strerror.a"
    )
endif()
# build_wasm.sh rewrites OUTPUT_NAME and -sEXPORT_NAME per-variant.
set_target_properties(Rpcs3 PROPERTIES OUTPUT_NAME "Rpcs3Dyna")

target_link_options(Rpcs3 PRIVATE "--bind")
target_link_options(Rpcs3 PRIVATE "-sMODULARIZE=1")
target_link_options(Rpcs3 PRIVATE "-sEXPORT_ES6=1")

if(ENABLE_WEBGPU)
    target_compile_definitions(Rpcs3 PRIVATE HAS_WEBGPU)
    # emdawnwebgpu must be a COMPILE option too — the port supplies
    # webgpu/webgpu_cpp.h, not just the link-time runtime.
    target_compile_options(Rpcs3 PRIVATE "--use-port=emdawnwebgpu")
    target_link_options(Rpcs3 PRIVATE "-sEXPORTED_FUNCTIONS=['_main','_initVm','_initVmWebGPU','_recreateWebGPUDevice']")
    target_link_options(Rpcs3 PRIVATE "-sEXPORTED_RUNTIME_METHODS=['ccall','FS','HEAP8','HEAPU8','HEAP32','HEAPF32','WebGPU','PThread','print','printErr','out','err']")
else()
    target_link_options(Rpcs3 PRIVATE "-sEXPORTED_FUNCTIONS=['_main','_initVm']")
    target_link_options(Rpcs3 PRIVATE "-sEXPORTED_RUNTIME_METHODS=['ccall','FS','HEAP8','HEAPU8','HEAP32','HEAPF32']")
endif()

target_link_options(Rpcs3 PRIVATE "-sFORCE_FILESYSTEM")
target_link_options(Rpcs3 PRIVATE "-sALLOW_TABLE_GROWTH")
target_link_options(Rpcs3 PRIVATE "-sUSE_ZLIB=1")

# Memory mode — build_wasm.sh swaps between fixed/growable per variant.
target_link_options(Rpcs3 PRIVATE "-sALLOW_MEMORY_GROWTH=1")
# Wasm32 hard ceiling is 4 GiB. We don't ship a build that needs more
# than the wasm32 max, so set MAXIMUM_MEMORY explicitly to 4 GiB so the
# runtime allows growth all the way up (default may cap lower on some
# emscripten releases, causing "Cannot enlarge memory arrays" aborts
# inside __wasm_call_ctors before main() — see vm::memory_reserve_4GiB
# which needs a 1.5 GiB aligned_alloc at static-init).
target_link_options(Rpcs3 PRIVATE "-sMAXIMUM_MEMORY=4294967296")
target_link_options(Rpcs3 PRIVATE "-sASSERTIONS=2")
target_link_options(Rpcs3 PRIVATE "-sSTACK_OVERFLOW_CHECK=2")
# target_link_options(Rpcs3 PRIVATE "-sALLOW_MEMORY_GROWTH=0")

# When HAS_EMUCORE is on the linked WASM is ~56 MB before runtime
# allocations — bump initial heap to 128 MB so wasm-ld doesn't reject
# the link with "initial memory too small". Growable still applies; this
# is just the starting size.
if(HAS_EMUCORE)
    target_link_options(Rpcs3 PRIVATE "-sINITIAL_MEMORY=134217728")
endif()

# Memory variants (only one uncommented at a time; build_wasm.sh manages):
# 512MB
# target_link_options(Rpcs3 PRIVATE "-sINITIAL_MEMORY=536870912")
# 1024MB (default — fits PS3 main RAM + VRAM + JIT + Emscripten overhead)
# target_link_options(Rpcs3 PRIVATE "-sINITIAL_MEMORY=1073741824")
# 2048MB
# target_link_options(Rpcs3 PRIVATE "-sINITIAL_MEMORY=2147483648")

option(OPTIMIZE_SIZE "Optimize for code size (-Oz)" OFF)
if(OPTIMIZE_SIZE)
    target_compile_options(Rpcs3 PRIVATE -Oz)
    target_link_options(Rpcs3 PRIVATE -Oz)
endif()

option(GSH_WEBGPU_VERBOSE_LOG "Compile WGPU_LOG printfs into the WebGPU backend (debug only)" ON)
if(GSH_WEBGPU_VERBOSE_LOG)
    target_compile_definitions(Rpcs3 PRIVATE GSH_WEBGPU_VERBOSE_LOG=1)
endif()

target_link_options(Rpcs3 PRIVATE "-sEXPORT_NAME=Rpcs3Dyna")
# -pthread must be set at COMPILE time too (not just link) — otherwise
# std::thread silently runs the callable inline on the calling thread,
# which deadlocks any worker that needs MAIN_THREAD_EM_ASM to proxy
# back to main (Phase 2 disc-bridge diagnostic, every future worker).
target_compile_options(Rpcs3 PRIVATE "-pthread")
target_link_options(Rpcs3 PRIVATE "-pthread")
# WASM tail-calls: the PPU interpreter dispatches each instruction via a tail-call chain
# (PPUInterpreter.cpp RETURN_ macro → `return fn(...)`). Native optimizes this to a sibling
# call (no stack growth); WASM without -mtail-call recurses one JS call frame per instruction,
# so a long basic block overflows ("Maximum call stack size exceeded" — hit by Dragon's Crown's
# boot, not Terraria's shorter blocks). -mtail-call makes the backend emit `return_call`.
target_compile_options(Rpcs3 PRIVATE "-mtail-call")
target_link_options(Rpcs3 PRIVATE "-mtail-call")
# Tried UBSan (-fsanitize=undefined): crash is in pthread bootstrap
# BEFORE UBSan runtime initializes, so no diagnostic improvement.
# Reverted.
# target_compile_options(Rpcs3 PRIVATE "-fsanitize=undefined")
# target_link_options(Rpcs3 PRIVATE "-fsanitize=undefined")
# Keep function names in the WASM name section so wasm-objdump (and
# the browser stack trace) shows meaningful symbols instead of
# function[157]. Wins us debug visibility for the wasm32 runtime
# alignment audit. Adds maybe ~5% to wasm size, no runtime cost.
target_link_options(Rpcs3 PRIVATE "--profiling-funcs")
# Pool size 64 — PS3 games typically have 20-30 threads (PPU game
# thread + audio + RSX + network + per-SPU). With pool=8 the boot
# logs hit "Tried to spawn a new thread, but the thread pool is
# exhausted" and the pthread call returns EAGAIN, halting the lv2
# scheduler. ALLOW_NEW_THREADS=1 (default) lets us grow if needed.
target_link_options(Rpcs3 PRIVATE "-sPTHREAD_POOL_SIZE=64")
target_link_options(Rpcs3 PRIVATE "-sPTHREAD_POOL_SIZE_STRICT=0")
# PTHREADS_DEBUG=1 was useful for the diagnostic in iteration v28 but
# spams console for every worker message; disabled. Re-enable when
# debugging pthread issues by uncommenting.
# target_link_options(Rpcs3 PRIVATE "-sPTHREADS_DEBUG=1")
# rpcs3 named_thread defaults to 8 MiB stack on native. Default
# emscripten pthread stack is 64 KiB — too small. Bump to 4 MiB.
target_link_options(Rpcs3 PRIVATE "-sDEFAULT_PTHREAD_STACK_SIZE=4194304")
target_link_options(Rpcs3 PRIVATE "-sSTACK_SIZE=4194304")
# Tried PROXY_TO_PTHREAD: didn't fix the pthread bootstrap crash and
# added "program exited" warnings. Reverted.
# target_link_options(Rpcs3 PRIVATE "-sPROXY_TO_PTHREAD=1")
target_link_options(Rpcs3 PRIVATE "-sWASM_BIGINT")
target_link_options(Rpcs3 PRIVATE "-sOFFSCREEN_FRAMEBUFFER=1")
target_link_options(Rpcs3 PRIVATE "-sENVIRONMENT=web,worker")
target_link_options(Rpcs3 PRIVATE "-sUSE_WEBGL2=1")
if(ENABLE_WEBGPU)
    target_link_options(Rpcs3 PRIVATE "--use-port=emdawnwebgpu")
endif()
