#!/bin/sh
set -e
IMAGE=quay.io/pypa/manylinux2014_i686:2026.11.01-2
SRC=$(cd "$(dirname "$0")/.." && pwd)
SDK=$(cd "${FASTDL_MM_SDK_ROOT:-$SRC/../hitbox_fixer/include}" && pwd)
RUNTIME=${CONTAINER_RUNTIME:-$(command -v podman || command -v docker)}

"$RUNTIME" run --rm --platform linux/386 \
    -v "$SRC":/src -v "$SDK":/sdk:ro -w /src "$IMAGE" \
    sh -c 'cmake -S . -B build/manylinux -DCMAKE_BUILD_TYPE=Release -DFASTDL_MM_SDK_ROOT=/sdk &&
           cmake --build build/manylinux -j"$(nproc)" &&
           ./build/manylinux/fastdl_tests'
echo "$SRC/build/manylinux/fastdl_mm_i386.so"
