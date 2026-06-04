#!/bin/bash
set -e
cd /Users/bank23232525/Documents/works/emu/rpcs3
source /Users/bank23232525/emsdk/emsdk_env.sh > /dev/null 2>&1
echo "=== recompiling cellFiber.cpp ($(date)) ==="
python3 << 'PYEOF'
import json, subprocess, sys
d = json.load(open('build_cmake/build/root-wasm/compile_commands.json'))
for e in d:
    if e['file'].endswith('Modules/cellFiber.cpp'):
        print("[recompile] cellFiber.cpp", flush=True)
        sys.exit(subprocess.run(e['command'], shell=True, cwd=e['directory']).returncode)
print("NOT FOUND"); sys.exit(1)
PYEOF
echo "=== updating .a + relinking ($(date)) ==="
emar r build_cmake/build/root-wasm/rpcs3/Emu/librpcs3_emu.a \
  build_cmake/build/root-wasm/rpcs3/Emu/CMakeFiles/rpcs3_emu.dir/Cell/Modules/cellFiber.cpp.o
./build_wasm.sh 2048
echo "=== DONE ($(date)) ==="
