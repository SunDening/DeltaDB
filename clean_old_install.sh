#!/bin/bash
# 删除旧的 /usr/local/include/deltadb 安装头文件
# 这些头文件使用的是旧的短路径 include 格式，会和新格式冲突

echo "正在删除旧的 DeltaDB 安装文件..."
sudo rm -rf /usr/local/include/deltadb
sudo rm -f /usr/local/lib/libDeltaDB*.a
sudo rm -f /usr/local/lib/libdb.a
sudo rm -f /usr/local/lib/libtable.a
sudo rm -f /usr/local/lib/libwal.a
sudo rm -f /usr/local/lib/libutils.a
echo "删除完成！现在可以重新编译项目。"
