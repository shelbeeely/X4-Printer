#!/bin/bash
set -euo pipefail

# Claude Code on the web clones this repo fresh into a container, and a
# plain `git clone` never populates submodule contents. firmware/freeink-sdk
# is a git submodule (see .gitmodules) that firmware/platformio.ini's
# `symlink://freeink-sdk/...` lib_deps require to exist on disk -- without
# this, every future session starts with the same "SDK not vendored, can't
# even read the headers" gap this hook exists to close.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

cd "$CLAUDE_PROJECT_DIR"
git submodule update --init --recursive
