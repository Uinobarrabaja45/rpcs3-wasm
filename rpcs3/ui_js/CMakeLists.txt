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
)
if(ENABLE_WEBGPU)
    list(APPEND RPCS3_JS_SOURCES GSH_WebGPUJs.cpp GSH_WebGPUJs.h)
endif()

add_executable(Rpcs3 ${RPCS3_JS_SOURCES})
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
    target_link_options(Rpcs3 PRIVATE "-sEXPORTED_RUNTIME_METHODS=['ccall','FS','HEAP8','HEAPU8','HEAP32','HEAPF32','WebGPU']")
else()
    target_link_options(Rpcs3 PRIVATE "-sEXPORTED_FUNCTIONS=['_main','_initVm']")
    target_link_options(Rpcs3 PRIVATE "-sEXPORTED_RUNTIME_METHODS=['ccall','FS','HEAP8','HEAPU8','HEAP32','HEAPF32']")
endif()

target_link_options(Rpcs3 PRIVATE "-sFORCE_FILESYSTEM")
target_link_options(Rpcs3 PRIVATE "-sALLOW_TABLE_GROWTH")

# Memory mode — build_wasm.sh swaps between fixed/growable per variant.
target_link_options(Rpcs3 PRIVATE "-sALLOW_MEMORY_GROWTH=1")
# target_link_options(Rpcs3 PRIVATE "-sALLOW_MEMORY_GROWTH=0")

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
target_link_options(Rpcs3 PRIVATE "-sPTHREAD_POOL_SIZE=4")
target_link_options(Rpcs3 PRIVATE "-sWASM_BIGINT")
target_link_options(Rpcs3 PRIVATE "-sOFFSCREEN_FRAMEBUFFER=1")
target_link_options(Rpcs3 PRIVATE "-sENVIRONMENT=web,worker")
target_link_options(Rpcs3 PRIVATE "-sUSE_WEBGL2=1")
if(ENABLE_WEBGPU)
    target_link_options(Rpcs3 PRIVATE "--use-port=emdawnwebgpu")
endif()
