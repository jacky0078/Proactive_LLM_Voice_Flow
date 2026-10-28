#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export EDGE_BUILD_DIR=${EDGE_BUILD_DIR:-"$project_dir/build/cockpit"}
exec "$project_dir/scripts/build_voice.sh" -DEDGE_BUILD_COCKPIT=ON "$@"
