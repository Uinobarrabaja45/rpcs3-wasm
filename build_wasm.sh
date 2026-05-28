#!/bin/bash
# Build RPCS3 WASM variants and deploy to js/rpcs3_browser
#
# Usage:
#   ./build_wasm.sh                 # build all default variants
#   ./build_wasm.sh 1024            # just Rpcs31024
#   ./build_wasm.sh Dyna            # just Rpcs3Dyna (growable memory)
#   ./build_wasm.sh 1024 Dyna       # both
#
# Recognised variants: 512, 1024, 2048, Dyna
#
# PS3 baseline memory: 256 MB main RAM + 256 MB RSX VRAM = 512 MB. With
# JIT translation cache + Emscripten/V8 overhead, 1 GB is the comfortable
# floor — that's the default. 2 GB gives commercial games headroom; Dyna
# starts small and grows for memory-constrained devices.
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Output directory of the fallback emcmake path (configures rpcs3/ui_js
# directly so output is flat, no Release/ subdir). The preset path puts
# artifacts in rpcs3/ui_js/Release/ — we'd need a second BUILD_DIR for that
# branch if/when we add the wasm-ninja preset to CMakePresets.json.
BUILD_DIR="$PROJECT_DIR/build_cmake/build/wasm-ninja"
JS_SRC="$PROJECT_DIR/js/rpcs3_browser/src"
JS_PUB="$PROJECT_DIR/js/rpcs3_browser/public"
CMAKE_LISTS="$PROJECT_DIR/rpcs3/ui_js/CMakeLists.txt"
CMAKE="${CMAKE:-$(command -v cmake)}"

if [ -z "$CMAKE" ] || [ ! -x "$CMAKE" ]; then
    echo "ERROR: cmake not found. Set CMAKE=/path/to/cmake or install it." >&2
    exit 1
fi

VARIANTS=("$@")
if [ ${#VARIANTS[@]} -eq 0 ]; then
    VARIANTS=("512" "1024" "2048" "Dyna")
fi

# Memory sizes per variant in bytes. Dyna uses growable memory and has no
# fixed INITIAL_MEMORY — return 0 as a sentinel; the loop handles it.
memory_for() {
    case "$1" in
        512)  echo 536870912 ;;   # 0.5 GB
        1024) echo 1073741824 ;;  # 1.0 GB (default)
        2048) echo 2147483648 ;;  # 2.0 GB
        Dyna) echo 0 ;;
        *) echo "Unknown variant: $1" >&2; exit 1 ;;
    esac
}

# WebGPU: ON for every variant (this is a WebGPU POC — that's the point).
# Kept as a function so future low-memory variants can opt out cleanly.
webgpu_for() {
    case "$1" in
        *) echo "ON" ;;
    esac
}

# Size optimization: enable for 512 MB build to keep the per-worker JIT
# cache footprint within budget on low-RAM devices.
optimize_size_for() {
    case "$1" in
        512) echo "ON" ;;
        *)   echo "OFF" ;;
    esac
}

# Backup CMakeLists.txt, restore on exit (even on Ctrl+C / error).
BACKUP=$(mktemp)
cp "$CMAKE_LISTS" "$BACKUP"
trap 'cp "$BACKUP" "$CMAKE_LISTS"; rm -f "$BACKUP"; echo "CMakeLists.txt restored."' EXIT

patch_cmake() {
    local name="$1"     # e.g. Rpcs31024 or Rpcs3Dyna
    local memory="$2"   # bytes, or 0 for dynamic (growable) memory
    local dynamic="$3"  # "1" = growable memory (no INITIAL_MEMORY), else fixed

    python3 - "$CMAKE_LISTS" "$name" "$memory" "$dynamic" <<'PYEOF'
import sys, re

path, name, memory, dynamic = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
dynamic_mode = (dynamic == "1")

with open(path) as f:
    content = f.read()

# Fix OUTPUT_NAME
content = re.sub(
    r'(set_target_properties\(Rpcs3 PROPERTIES OUTPUT_NAME )"Rpcs3\w+"',
    rf'\1"{name}"',
    content
)

# Fix EXPORT_NAME
content = re.sub(
    r'(-sEXPORT_NAME=)Rpcs3\w+"',
    rf'\g<1>{name}"',
    content
)

# Comment out ALL INITIAL_MEMORY lines (active form → commented).
content = re.sub(
    r'^target_link_options\(Rpcs3 PRIVATE "-sINITIAL_MEMORY=(\d+)"\)',
    r'# target_link_options(Rpcs3 PRIVATE "-sINITIAL_MEMORY=\1")',
    content,
    flags=re.MULTILINE
)

if dynamic_mode:
    # Growable: leave all INITIAL_MEMORY commented, ALLOW_MEMORY_GROWTH=1
    content = re.sub(
        r'^target_link_options\(Rpcs3 PRIVATE "-sALLOW_MEMORY_GROWTH=0"\)',
        r'# target_link_options(Rpcs3 PRIVATE "-sALLOW_MEMORY_GROWTH=0")',
        content,
        flags=re.MULTILINE
    )
    content = content.replace(
        '# target_link_options(Rpcs3 PRIVATE "-sALLOW_MEMORY_GROWTH=1")',
        'target_link_options(Rpcs3 PRIVATE "-sALLOW_MEMORY_GROWTH=1")',
        1
    )
else:
    # Fixed: uncomment the matching INITIAL_MEMORY. KEEP growth=1 so the
    # wasm32 vm layer's 1.5 GiB g_base_addr aligned_alloc at static-init
    # can succeed even on the smaller (1024, 512) builds — INITIAL_MEMORY
    # is now just the *starting* size, and MAXIMUM_MEMORY=4 GiB (set in
    # CMakeLists.txt) is the hard cap. Previously growth was disabled,
    # which made the 1024 build abort during __wasm_call_ctors trying to
    # grow to 1.6 GiB. Fixed builds still have a predictable starting
    # heap; only the cap on resize is what changes.
    target_line = f'# target_link_options(Rpcs3 PRIVATE "-sINITIAL_MEMORY={memory}")'
    active_line = f'target_link_options(Rpcs3 PRIVATE "-sINITIAL_MEMORY={memory}")'
    content = content.replace(target_line, active_line, 1)
    # Leave ALLOW_MEMORY_GROWTH=1 alone (it's already enabled in
    # CMakeLists.txt). Do NOT disable growth.

with open(path, 'w') as f:
    f.write(content)
PYEOF
}

copy_outputs() {
    local name="$1"
    mkdir -p "$JS_SRC" "$JS_PUB"
    for dest in "$JS_SRC" "$JS_PUB"; do
        cp "$BUILD_DIR/${name}.js"   "$dest/${name}.js"
        cp "$BUILD_DIR/${name}.wasm" "$dest/${name}.wasm"
    done
    echo "  → Deployed ${name}.js + ${name}.wasm to js/rpcs3_browser/{src,public}/"
}

# Resolve the wasm-ninja preset. If CMakePresets.json has no wasm-ninja
# entry yet, fall back to a manual emcmake invocation. Either way the
# binary dir winds up at $BUILD_DIR.
configure_and_build() {
    local webgpu="$1"
    local optsize="$2"

    CMAKE_EXTRA_FLAGS=""
    if [ "$optsize" = "ON" ]; then
        CMAKE_EXTRA_FLAGS="-DCMAKE_C_FLAGS_RELEASE=-Oz -DCMAKE_CXX_FLAGS_RELEASE=-Oz"
    fi

    if "$CMAKE" --list-presets=configure 2>/dev/null | grep -q wasm-ninja; then
        "$CMAKE" --preset wasm-ninja \
            -DENABLE_WEBGPU="${webgpu}" \
            -DOPTIMIZE_SIZE="${optsize}" \
            ${CMAKE_EXTRA_FLAGS} 2>&1 | tail -5
        "$CMAKE" --build --preset wasm-ninja-release --target Rpcs3
    else
        # Fallback: assume emsdk is sourced (EMSDK env var set) and emcmake
        # is on PATH. Invoke directly against rpcs3/ui_js so we don't drag
        # in the full rpcs3 build (which can't cross-compile to WASM yet).
        : "${EMSDK:?EMSDK env var not set — run \"source emsdk_env.sh\" first}"
        local BIN_DIR="$PROJECT_DIR/build_cmake/build/wasm-ninja"
        mkdir -p "$BIN_DIR"
        ( cd "$BIN_DIR" && emcmake "$CMAKE" \
            -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DENABLE_WEBGPU="${webgpu}" \
            -DOPTIMIZE_SIZE="${optsize}" \
            -DHAS_EMUCORE=ON \
            ${CMAKE_EXTRA_FLAGS} \
            "$PROJECT_DIR/rpcs3/ui_js" )
        "$CMAKE" --build "$BIN_DIR" --target Rpcs3
    fi
}

# Build each requested variant
for variant in "${VARIANTS[@]}"; do
    name="Rpcs3${variant}"
    memory=$(memory_for "$variant")
    webgpu=$(webgpu_for "$variant")
    optsize=$(optimize_size_for "$variant")

    if [ "$variant" = "Dyna" ]; then
        dynamic=1
        mem_display="dynamic"
    else
        dynamic=0
        mem_display="$((memory / 1024 / 1024))MB"
    fi

    echo ""
    echo "=============================="
    echo "  Building $name  (${mem_display}, WebGPU=${webgpu}, Oz=${optsize})"
    echo "=============================="

    # Restore from backup before patching (clean slate each iteration)
    cp "$BACKUP" "$CMAKE_LISTS"
    patch_cmake "$name" "$memory" "$dynamic"

    configure_and_build "$webgpu" "$optsize"

    wasm_path="$BUILD_DIR/${name}.wasm"
    if [ -f "$wasm_path" ]; then
        wasm_size=$(wc -c < "$wasm_path")
        echo "  WASM size: $((wasm_size / 1024))KB"
        copy_outputs "$name"
        echo "  $name done."
    else
        echo "  WARNING: $wasm_path not found — build may have failed silently."
    fi
done

echo ""
echo "All done: ${VARIANTS[*]/#/Rpcs3}"
echo ""
echo "Next: cd js/rpcs3_browser && npm install && npm start"
echo "Then open http://localhost:3030/?ram=1024&webgpu=true"
