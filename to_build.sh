#!/bin/bash

set -euo pipefail  # 更严格的错误处理：-u 检查未定义变量，-o pipefail 管道错误传递

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "🔨 DeltaDB 构建脚本"
echo "===================="

# 检查依赖命令
command -v cmake >/dev/null 2>&1 || { echo -e "${RED}错误: cmake 未安装${NC}" >&2; exit 1; }
command -v make >/dev/null 2>&1 || { echo -e "${RED}错误: make 未安装${NC}" >&2; exit 1; }

# 获取 CPU 核心数（兼容 macOS 和 Linux）
if command -v nproc >/dev/null 2>&1; then
    JOBS=$(nproc)
elif command -v sysctl >/dev/null 2>&1; then
    JOBS=$(sysctl -n hw.ncpu)
else
    JOBS=4
    echo -e "${YELLOW}警告: 无法检测 CPU 核心数，默认使用 ${JOBS} 线程${NC}"
fi

# 清理旧的构建文件（如果存在）
echo "🧹 清理旧构建文件..."
if [ -d "${BUILD_DIR}" ]; then
    # 安全清理：只删除特定文件/目录，保留 _deps（FetchContent 下载的依赖）
    cd "${BUILD_DIR}"
    
    # 使用数组和循环安全删除
    items_to_remove=("CMakeCache.txt" "CMakeFiles" "bin" "lib" "Makefile" "cmake_install.cmake" "CTestTestfile.cmake")
    
    for item in "${items_to_remove[@]}"; do
        if [ -e "${item}" ]; then
            rm -rf "${item}"
            echo "  已删除: ${item}"
        fi
    done
    
    cd "${SCRIPT_DIR}"
else
    echo "  创建 build 目录"
    mkdir -p "${BUILD_DIR}"
fi

# 进入构建目录
cd "${BUILD_DIR}"

# 运行 CMake
echo "⚙️  运行 CMake..."
if ! cmake .. -DCMAKE_BUILD_TYPE=Release; then
    echo -e "${RED}错误: CMake 配置失败${NC}" >&2
    exit 1
fi

# 编译
echo "🔧 开始编译（使用 ${JOBS} 线程）..."
if ! make -j"${JOBS}"; then
    echo -e "${RED}错误: 编译失败${NC}" >&2
    exit 1
fi

# 检查输出文件
echo "📦 检查构建结果..."
if [ -f "bin/deltadb" ]; then
    echo -e "  ${GREEN}✓ 主程序: bin/deltadb${NC}"
else
    echo -e "  ${YELLOW}⚠ 主程序未生成${NC}"
fi

if [ -f "bin/deltadb_test" ]; then
    echo -e "  ${GREEN}✓ 测试程序: bin/deltadb_test${NC}"
else
    echo -e "  ${YELLOW}⚠ 测试程序未生成${NC}"
fi

echo ""
echo -e "${GREEN}🎉 构建完成！${NC}"
echo ""
echo "使用方式:"
echo "  运行主程序:  ./build/bin/deltadb"
echo "  运行测试:    ./build/bin/deltadb_test"
echo "  清理构建:    rm -rf build"