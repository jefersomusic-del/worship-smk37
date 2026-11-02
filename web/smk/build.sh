#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# WORSHIP's browser simulator: the SMK-37 build (web/emu/smk_web.c) compiled to WebAssembly with plain clang (no
# Emscripten, no libc: web/smk/libc's headers, web/smk/wasm_libc.c), embedded in one page.
#   web/smk/build.sh  ->  build/smk/worship-sim.html   (after the generators once: build/gen)
set -e
cd "$(dirname "$0")/../.."
[ -f build/gen/felucca_tables.h ] || { echo "smk sim: build/gen first (tools/build.py generate)"; exit 1; }
mkdir -p build/smk
CC=${CC:-clang}
$CC --target=wasm32 -O2 -ffp-contract=off -std=gnu99 -w -nostdlib -isystem web/smk/libc -Ibuild/gen -Ifirmware/src \
    -Ifirmware/hal '-DFELUCCA_VERSION="WORSHIP"' -c web/emu/smk_web.c -o build/smk/smk_web.o
$CC --target=wasm32 -O2 -nostdlib -c web/smk/wasm_libc.c -o build/smk/libc.o
wasm-ld --no-entry --export-dynamic --allow-undefined -z stack-size=1048576 --initial-memory=67108864 \
    -o build/smk/smk.wasm build/smk/smk_web.o build/smk/libc.o
python3 - <<'EOF'
import base64
w = base64.b64encode(open("build/smk/smk.wasm", "rb").read()).decode()
html = open("web/smk/index.html").read().replace("__WASM_BASE64__", w)
open("build/smk/worship-sim.html", "w").write(html)
print("smk sim: build/smk/worship-sim.html", len(html), "B")
EOF
