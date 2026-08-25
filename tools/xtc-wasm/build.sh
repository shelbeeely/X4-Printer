#!/usr/bin/env bash
# Builds xtc_decoder.cpp to WASM and copies the output into
# firmware/src/ui/wasm/, where WebUiServer.cpp's generate_header.py turns
# it into an embeddable C++ header. Requires emscripten (`apt-get install
# -y emscripten`, or the emsdk — see README.md in this directory).
#
# Run this after changing xtc_decoder.cpp, then re-run
# firmware/src/ui/wasm/generate_header.py and commit both the rebuilt
# .wasm/.js here and the regenerated header. CI (.github/workflows/
# tests.yml) rebuilds from scratch and fails if either has drifted from
# what's committed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../../firmware/src/ui/wasm"
mkdir -p "$OUT_DIR"

emcc "$SCRIPT_DIR/xtc_decoder.cpp" \
  -O2 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=createXtcDecoderModule \
  -s EXPORTED_FUNCTIONS='["_xtc_decode_page","_xtc_page_count","_xtc_free","_malloc","_free"]' \
  -s EXPORTED_RUNTIME_METHODS='["HEAPU8","HEAPU16","HEAPU32","setValue","getValue"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s ENVIRONMENT=web \
  -s SINGLE_FILE=0 \
  -o "$OUT_DIR/xtc_decoder.js"

echo "Built $OUT_DIR/xtc_decoder.wasm and $OUT_DIR/xtc_decoder.js"
ls -la "$OUT_DIR"/xtc_decoder.*
