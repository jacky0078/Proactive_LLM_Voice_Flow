#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${EDGE_BUILD_DIR:-"$project_dir/build/host"}
voice_deps_root=${VOICE_DEPS_ROOT:-"$project_dir/voice-deps"}
rkllm_root=${RKLLM_ROOT:-"$voice_deps_root/rkllm"}
cmake -S "$project_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release \
    -UVOICE_FLOW_ROOT -DVOICE_DEPS_ROOT="$voice_deps_root" -DRKLLM_ROOT="$rkllm_root" "$@"
cmake --build "$build_dir" -j "${EDGE_BUILD_JOBS:-4}"
(cd "$build_dir" && ctest --output-on-failure)
