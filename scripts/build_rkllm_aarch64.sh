#!/usr/bin/env bash
# Cross-build only edge_llm + its SDK/transport dependencies against the real RKLLM library.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${EDGE_RK_BUILD_DIR:-"$project_dir/build/aarch64"}
deps_dir=${EDGE_DEPS_DIR:-"$project_dir/build/deps"}
voice_deps_root=${VOICE_DEPS_ROOT:-"$project_dir/voice-deps"}
rkllm_root=${RKLLM_ROOT:-"$voice_deps_root/rkllm"}
zmq_prefix="$deps_dir/zmq-aarch64-install"
mkdir -p "$deps_dir"
archive="$deps_dir/libzmq-4.3.5.tar.gz"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 2 \
        https://codeload.github.com/zeromq/libzmq/tar.gz/refs/tags/v4.3.5 -o "$archive"
fi
printf '%s  %s\n' 6c972d1e6a91a0ecd79c3236f04cf0126f2f4dfbbad407d72b4606a7ba93f9c6 "$archive" | sha256sum --check
if [[ ! -d "$deps_dir/libzmq-4.3.5" ]]; then
    tar -xzf "$archive" -C "$deps_dir"
fi
cmake -S "$deps_dir/libzmq-4.3.5" -B "$deps_dir/zmq-aarch64" \
    -DCMAKE_TOOLCHAIN_FILE="$project_dir/cmake/aarch64-linux-gnu.cmake" \
    -DCMAKE_INSTALL_PREFIX="$zmq_prefix" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED=OFF -DBUILD_STATIC=ON -DBUILD_TESTS=OFF -DENABLE_CURVE=OFF \
    -DWITH_LIBSODIUM=OFF -DWITH_LIBBSD=OFF -DWITH_LIBGSSAPI_KRB5=OFF -DENABLE_DRAFTS=OFF
cmake --build "$deps_dir/zmq-aarch64" -j "${EDGE_BUILD_JOBS:-4}"
cmake --install "$deps_dir/zmq-aarch64"
cmake -S "$project_dir" -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$project_dir/cmake/aarch64-linux-gnu.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DZMQ_ROOT="$zmq_prefix" \
    -UVOICE_FLOW_ROOT -URKLLM_INCLUDE_DIR -URKLLM_LIBRARY \
    -DVOICE_DEPS_ROOT="$voice_deps_root" -DRKLLM_ROOT="$rkllm_root" \
    -DEDGE_BUILD_MANAGER=OFF -DEDGE_BUILD_ASR=OFF -DEDGE_BUILD_TTS=OFF \
    -DEDGE_ENABLE_RKLLM=ON -DBUILD_TESTING=OFF
cmake --build "$build_dir" -j "${EDGE_BUILD_JOBS:-4}"
cmake --install "$build_dir" --prefix "$build_dir/package"
readelf -h "$build_dir/package/bin/edge_llm" | tee "$build_dir/elf-header.txt"
readelf -d "$build_dir/package/bin/edge_llm" | tee "$build_dir/elf-dynamic.txt"
grep -q AArch64 "$build_dir/elf-header.txt"
grep -q 'librkllmrt.so' "$build_dir/elf-dynamic.txt"
readelf -Ws "$build_dir/package/bin/edge_llm" | grep -E 'UND.*rkllm_(init|run|abort|destroy)'
printf 'RKLLM package: %s\n' "$build_dir/package"
