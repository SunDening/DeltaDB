#!/bin/bash
set -e

BUILD_DIR="build"

# rm -rf output/delta

# 清理
if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"/*
else
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# 配置
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译
make -j$(nproc)

echo "Build complete!"
echo "Binary: ./build/bin/deltadb"