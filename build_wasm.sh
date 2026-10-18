#!/usr/bin/env bash
set -euo pipefail

output_dir="${1:-_site}"
mkdir -p "$output_dir"

em++ main.cpp -std=c++17 -O2 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s NO_EXIT_RUNTIME=1 \
  -s EXPORTED_FUNCTIONS='["_main","_wasm_get_state","_wasm_get_visible","_wasm_step","_wasm_reset","_wasm_set_spacing","_wasm_set_direction"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall"]' \
  -o "$output_dir/ant.js"

if [[ "$output_dir" != "." ]]; then
  cp index.html style.css app.js "$output_dir/"
fi
