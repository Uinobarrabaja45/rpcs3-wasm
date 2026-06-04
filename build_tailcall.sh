#!/bin/bash
# Targeted EMUCORE recompile: rebuild only PPUInterpreter.cpp + PPUThread.cpp with -mtail-call
# (so the PPU interpreter's per-instruction tail-call dispatch chain becomes wasm `return_call`,
# fixing Dragon's Crown's "Maximum call stack size exceeded" boot crash), update librpcs3_emu.a,
# then relink the wasm via build_wasm.sh.
set -e
cd /Users/bank23232525/Documents/works/emu/rpcs3
source /Users/bank23232525/emsdk/emsdk_env.sh > /dev/null 2>&1
echo "=== recompiling interpreter files with -mtail-call ($(date)) ==="
python3 << 'PYEOF'
import json, subprocess, sys
d = json.load(open('build_cmake/build/root-wasm/compile_commands.json'))
for t in ['Emu/Cell/PPUInterpreter.cpp', 'Emu/Cell/PPUThread.cpp']:
    for e in d:
        if e['file'].endswith(t):
            cmd = e['command'].replace('em++ ', 'em++ -mtail-call ', 1)
            print(f"[recompile] {t}", flush=True)
            r = subprocess.run(cmd, shell=True, cwd=e['directory'])
            if r.returncode != 0:
                print(f"FAILED: {t}", flush=True); sys.exit(1)
            break
    else:
        print(f"NOT FOUND: {t}", flush=True); sys.exit(1)
print("recompile OK", flush=True)
PYEOF
echo "=== updating librpcs3_emu.a ($(date)) ==="
emar r build_cmake/build/root-wasm/rpcs3/Emu/librpcs3_emu.a \
  build_cmake/build/root-wasm/rpcs3/Emu/CMakeFiles/rpcs3_emu.dir/Cell/PPUInterpreter.cpp.o \
  build_cmake/build/root-wasm/rpcs3/Emu/CMakeFiles/rpcs3_emu.dir/Cell/PPUThread.cpp.o
echo "=== relinking wasm via build_wasm.sh ($(date)) ==="
./build_wasm.sh 2048
echo "=== DONE ($(date)) ==="
