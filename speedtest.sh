#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "speedtest.sh is intended for Linux."
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-speedtest}"
JOBS="${JOBS:-$(nproc)}"

# Keep the release pipeline explicit and aggressive for quick speed checks.
C_FLAGS_RELEASE="${C_FLAGS_RELEASE:--O3 -DNDEBUG -march=native -mtune=native -flto}"
CXX_FLAGS_RELEASE="${CXX_FLAGS_RELEASE:--O3 -DNDEBUG -march=native -mtune=native -flto}"

GENERATOR_ARGS=()
if [[ ! -d "${BUILD_DIR}" ]] && command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

cmake \
    -S "${ROOT_DIR}" \
    -B "${BUILD_DIR}" \
    "${GENERATOR_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="${C_FLAGS_RELEASE}" \
    -DCMAKE_CXX_FLAGS_RELEASE="${CXX_FLAGS_RELEASE}"

cmake --build "${BUILD_DIR}" -j "${JOBS}" --target allocazam_malloc_benchmark

"${BUILD_DIR}/allocazam_malloc_benchmark" "$@"
