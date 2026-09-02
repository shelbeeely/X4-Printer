#!/usr/bin/env bash
# Builds xtc_encoder.cpp to WASM and copies the output into
# firmware/src/ui/wasm/, where firmware/src/ui/wasm/generate_encoder_header.py
# turns it into an embeddable C++ header. Requires emscripten (`apt-get
# install -y emscripten`, or the emsdk — see README.md in this directory).
# Mirrors build.sh (the decoder's own build script) exactly, one module at
# a time -- these are two separate WASM binaries, not one combined build.
#
# Run this after changing xtc_encoder.cpp, then re-run
# firmware/src/ui/wasm/generate_encoder_header.py and commit both the
# rebuilt .wasm/.js here and the regenerated header. CI (.github/
# workflows/tests.yml) rebuilds from scratch and fails if either has
# drifted from what's committed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../../firmware/src/ui/wasm"
mkdir -p "$OUT_DIR"

emcc "$SCRIPT_DIR/xtc_encoder.cpp" \
  -O2 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=createXtcEncoderModule \
  -s EXPORTED_FUNCTIONS='["_xtc_encode_single_page","_xtc_free","_malloc","_free"]' \
  -s EXPORTED_RUNTIME_METHODS='["HEAPU8","HEAPU16","HEAPU32","setValue","getValue"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s ENVIRONMENT=web \
  -s SINGLE_FILE=0 \
  -o "$OUT_DIR/xtc_encoder.js"

echo "Built $OUT_DIR/xtc_encoder.wasm and $OUT_DIR/xtc_encoder.js"
ls -la "$OUT_DIR"/xtc_encoder.*
