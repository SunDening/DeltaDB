#!/usr/bin/env bash

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

if command -v nproc >/dev/null 2>&1; then
    JOBS="${JOBS:-$(nproc)}"
elif command -v getconf >/dev/null 2>&1; then
    JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
else
    JOBS="${JOBS:-4}"
fi

rm -rf "${PROJECT_ROOT}/output"/*

if [[ -d "${BUILD_DIR}" ]]; then
    rm -rf "${BUILD_DIR}"/*
else
    mkdir -p "${BUILD_DIR}"
fi

echo "Configuring development build in ${BUILD_DIR} ..."
echo "  app:   ON"
echo "  tests: ON"
cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DDELTA_DB_BUILD_APP=ON \
    -DDELTA_DB_BUILD_TESTS=ON

echo "Building DeltaDB app and tests ..."
cmake --build "${BUILD_DIR}" -j"${JOBS}"

echo "Build complete!"
echo "App binary:  ${BUILD_DIR}/bin/deltadb"
echo "Test binary: ${BUILD_DIR}/bin/my_test"
