#pragma once

/**
 * WAL 格式定义
 * .wal 文件命名规则：{数据库目录}/{6位数字编号}.log。如：/data/mydb/000123.log
 * WAL 文件为三层结构层次：
 *  1. 文件级别：WAL 文件本身，如000001.wal
 *  2. 块级别：固定 32KB 大小
 *  3. 记录级别：7字节头部 + 数据体
 * 每个 WAL 文件由若干个块（Block）组成，而每个块又由若干条记录（Record）组成:
    WAL 文件
    └── Block 1 (32KB)
            ├── Record 1
            ├── Record 2
            └── ...
    └── Block 2 (32KB)
            ├── Record N
            └── ...
    └── Block 3 (最后可能不足32KB)
            └── Record M
 * 单个记录的格式定义在此文件中:
    记录 = [7字节头部] + [数据体]

    头部：
        checksum (4字节)  : uint32, CRC32C 校验码，小端序
        length   (2字节)  : uint16, 数据长度，小端序
        type     (1字节)  : uint8, 记录类型
    数据：
        data     (N字节)   : 用户数据

 * 记录分片规则：
    当记录跨越块边界时会被分片，且记录永远不会在块的最后6个字节内开始（因为至少需要7字节头部），这些剩余字节形成尾部，必须全是0:
        用户记录：
            A: 1000 字节
            B: 97270 字节
            C: 8000 字节
        存储方式：
            Block 1: [FULL A] [FIRST B(剩余空间)]
            Block 2: [MIDDLE B(完整块)]
            Block 3: [LAST B] [6字节尾部]
            Block 4: [FULL C]

 * WAL 文件切换机制：WAL 文件切换不是基于文件大小限制，而是基于 MemTable 刷盘
    ? 无固定大小限制：WAL 文件大小取决于写入的数据量和 write_buffer_size 配置
    ? 按需切换：每次 MemTable 刷盘时创建新的 WAL 文件
    ? 安全删除：只删除已确保数据持久化的旧 WAL 文件
    ? 双文件保护：始终保留当前和上一个 WAL 文件，提供额外安全层

    写入 → WAL → MemTable
                ↓
            MemTable 满
                ↓
            切换新 WAL
                ↓
            旧 WAL + 旧 MemTable → SSTable
                ↓
            删除旧 WAL
 */

namespace delta {

/**
 * 标识记录类型，用于分片重组
 */
enum RecordType {
    kZeroType = 0,    // 保留. 预分配文件时填充使用
    kFullType = 1,    // 完整记录（未分片）
    kFirstType = 2,   // 分片的第一个片段（处理跨块的记录分片）
    kMiddleType = 3,  // 分片的中间片段（处理跨块的记录分片）
    kLastType = 4     // 分片的最后一个片段（处理跨块的记录分片）
};

// 最大记录类型值
static const int kMaxRecordType = kLastType;

// 块大小：32KB. 固定块大小，便于对齐和恢复
static const int kBlockSize = 32768;

// 每条记录头部大小：7字节（4字节CRC + 2字节长度 + 1字节类型）
static const int kHeaderSize = 4 + 2 + 1;
}  // namespace delta