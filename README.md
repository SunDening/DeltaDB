# DeltaDB

> 一个受 LevelDB 启发的 C++20 嵌入式键值存储引擎，采用 LSM-Tree 架构，支持 WAL、SSTable、多级 Compaction、快照隔离和 Bloom Filter 等特性。

**版本**: 1.23 | **语言**: C++20 | **架构**: LSM-Tree

---

## 目录

- [项目简介](#项目简介)
- [核心特性](#核心特性)
- [架构总览](#架构总览)
- [模块详解](#模块详解)
  - [Utils 工具模块](#1-utils-工具模块)
  - [WAL 预写日志模块](#2-wal-预写日志模块)
  - [Table SSTable 模块](#3-table-sstable-模块)
  - [DB 数据库核心模块](#4-db-数据库核心模块)
- [数据格式设计](#数据格式设计)
- [写入路径](#写入路径)
- [读取路径](#读取路径)
- [Compaction 机制](#compaction-机制)
- [构建与运行](#构建与运行)
- [配置说明](#配置说明)
- [技术亮点](#技术亮点)

---

## 项目简介

DeltaDB 是一个高性能嵌入式键值存储引擎，采用 **LSM-Tree（Log-Structured Merge Tree）** 架构设计。项目整体受 LevelDB 启发，使用 C++20 标准实现，代码遵循 Google 编码规范（clang-format 格式化，120 列宽限制，4 空格缩进）。

DeltaDB 适用于需要持久化存储、高吞吐写入的场景，如日志存储、时序数据、缓存后端等。作为嵌入式数据库，它直接链接到应用程序中，无需独立的数据库服务器。

---

## 核心特性

| 特性 | 说明 |
|------|------|
| **LSM-Tree 架构** | 7 层结构（L0-L6），MemTable → SSTable → 多级 Compaction |
| **WAL 持久化** | 32KB 块对齐的预写日志，CRC32C 校验，支持分片记录 |
| **快照隔离** | 基于 MVCC 的快照机制，56 位序列号保证一致性读 |
| **Bloom Filter** | 双层哈希布隆过滤器，大幅减少无效磁盘读取 |
| **前缀压缩** | SSTable Block 内键前缀压缩，有效降低存储占用 |
| **数据压缩** | 支持 Snappy 和 Zstd 压缩，压缩率不佳时自动回退 |
| **LRU 缓存** | 16 分片 LRU 缓存 Block 数据，线程安全高并发 |
| **原子批量写入** | WriteBatch 支持原子 Put/Delete 操作，WAL 合并写 |
| **异步日志** | 多级异步日志系统，支持按大小轮转和信号处理 |
| **跳表索引** | 无锁读取跳表（SkipList）作为 MemTable 后端 |

---

## 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                        DeltaDB                              │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────────┐  │
│  │   DB API    │  │  WriteBatch  │  │    Snapshot       │  │
│  │ Put/Get/Del │  │  Atomic Ops  │  │    MVCC Read      │  │
│  └──────┬──────┘  └──────┬───────┘  └────────┬──────────┘  │
│         │                │                    │             │
│  ┌──────▼────────────────▼────────────────────▼──────────┐  │
│  │                  DBImpl 核心实现                       │  │
│  │  ┌──────────┐  ┌──────────────┐  ┌─────────────────┐  │  │
│  │  │ MemTable │  │ Immutable MT │  │  Background     │  │  │
│  │  │ SkipList │  │   (readonly) │  │   Compaction    │  │  │
│  │  └────┬─────┘  └──────┬───────┘  └────────┬────────┘  │  │
│  │       │               │                    │           │  │
│  │  ┌────▼───────────────▼────────────────────▼────────┐  │  │
│  │  │              VersionSet 版本管理                  │  │  │
│  │  │  L0 (SST) → L1 (SST) → ... → L6 (SST)          │  │  │
│  │  └──────────────────────────────────────────────────┘  │  │
│  └───────────────────────┬─────────────────────────────────┘  │
│                          │                                     │
│  ┌───────────────────────▼─────────────────────────────────┐  │
│  │                    WAL 预写日志                          │  │
│  │  32KB Block | CRC32C | 分片记录 | fsync 保障            │  │
│  └─────────────────────────────────────────────────────────┘  │
│                                                                │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────────┐  │
│  │  Bloom Filter│  │  LRU Cache   │  │  SSTable 文件缓存  │  │
│  │  双哈希探测  │  │  16 分片     │  │  RandomAccessFile  │  │
│  └──────────────┘  └──────────────┘  └────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 模块详解

### 1. Utils 工具模块

基础工具集合，为整个项目提供底层支撑。

#### 1.1 Status 状态码

统一的错误处理机制，采用紧凑的状态编码格式：

- **状态码**: `OK`、`NotFound`、`Corruption`、`NotSupported`、`InvalidArgument`、`IOError`
- **编码格式**: 使用 4 字节 packed 状态码 + 可变长度消息，内存占用极小
- **工厂方法**: `Status::OK()`、`Status::NotFound(msg)` 等静态工厂方法

#### 1.2 Coding 编解码

提供固定宽度和可变长度整数编解码：

- **固定宽度**: `EncodeFixed32/64`、`DecodeFixed32/64`（小端序）
- **Varint**: `GetVarint32/64`、`PutVarint32/64`，小值仅占 1 字节，有效节省存储空间
- **LevelDB 兼容**: 编解码格式与 LevelDB 完全兼容

#### 1.3 Comparator 比较器

抽象比较接口，支持自定义排序规则：

- `BytewiseComparator`: 按字节序比较的默认实现
- `FindShortestSeparator`: 寻找两个键之间的最短分隔符（用于 Block 前缀压缩优化）
- `FindShortSuccessor`: 寻找键的最短后继（用于 SSTable 范围扫描优化）

#### 1.4 Arena 内存分配器

Bump-Pointer 分配器，专为小对象高频分配场景设计：

- **块分配**: 每次分配 4KB 内存块，减少系统调用
- **对齐分配**: 支持对齐内存分配，满足 SIMD 等场景需求
- **适用场景**: MemTable 中的键值对分配，避免频繁 malloc/free

#### 1.5 SkipList 跳表

LSM-Tree MemTable 的核心数据结构：

- **无锁读取**: 原子指针发布，读取端完全无锁
- **写外部同步**: 写入由外部锁保护，读并发无竞争
- **最大高度 12**: 概率性层级结构，搜索时间复杂度 O(log N)
- **不可变节点**: 节点一旦创建便不可修改，保证读取线程安全

#### 1.6 MySkipList 增强跳表

另一版本的跳表实现，使用 `shared_mutex` 读写锁：

- 支持插入/搜索/删除操作
- 基于概率的层级生成
- 适合需要并发删除的场景

#### 1.7 DBFormat 内部键格式

DeltaDB 的内部键格式设计是整个 MVCC 机制的基石：

```
┌──────────────────┬───────────────────┬──────────────┐
│    user_key      │  56-bit sequence  │  8-bit type  │
│  (变长用户键)    │    (MVCC 序列号)   │  (值类型)    │
└──────────────────┴───────────────────┴──────────────┘
```

- **InternalKeyComparator**: 先按 user_key 升序，再按 sequence 降序（保证相同 key 的最新版本排在最前）
- **InternalFilterPolicy**: 包装用户 Bloom Filter，适配内部键格式
- **LookupKey**: 查找键封装，方便 MemTable 快速定位

#### 1.8 Config 配置系统

基于 TinyXML 的 XML 配置解析：

- **配置项**: WAL 路径、Compaction 触发阈值、缓冲区大小、Block 大小、压缩设置、缓存大小、Bloom Filter 等
- **ReadOptions/WriteOptions**: 读写操作的运行时选项
- **Limiter**: 原子资源控制器，用于限制并发文件描述符等

#### 1.9 Logger 异步日志

多级异步日志系统：

- **日志级别**: DEBUG / INFO / WARN / ERROR
- **异步写入**: 独立线程缓冲写入，减少对主路径的影响
- **文件轮转**: 按文件大小自动轮转（默认 5MB）
- **信号处理**: 注册 coredump 信号处理器
- **线程安全**: 多线程安全的缓冲写入

#### 1.10 Bloom Filter 布隆过滤器

空间高效的概率型数据结构：

- **双哈希技巧**: 使用两个哈希函数模拟多个哈希，减少计算开销
- **可配置位数**: `NewBloomFilterPolicy(bits_per_key)` 控制误判率
- **应用场景**: SSTable 快速判断键是否不存在，避免无效磁盘 I/O

#### 1.11 工具函数集

- **文件操作**: `CreateDir`、`RenameFile`、`RemoveFile`、`WriteStringToFileSync`
- **压缩封装**: Snappy / Zstd 压缩/解压
- **随机数**: `Random`（线性同余 PRNG）
- **哈希**: Murmur 风格哈希函数
- **时间**: `NowMicros`、`SleepForMicroseconds`
- **同步原语**: `CondVar` 条件变量封装

#### 1.12 NoDestructor 防析构包装器

线程安全的单例包装器，避免静态对象销毁顺序问题。

#### 1.13 SingleThreadPool 单线程池

懒启动的单工作线程池，首次 `Schedule()` 时启动线程，用于后台 Compaction 任务。

---

### 2. WAL 预写日志模块

Write-Ahead Log 是保证数据持久化的核心机制。

#### 2.1 WAL 格式设计

```
┌─────────────────────────────────────────────────────────┐
│                    32KB Block                           │
├─────────────────────────────────────────────────────────┤
│  ┌────────────────────────────────────────────────────┐ │
│  │ Record Header (7 bytes)                            │ │
│  │ ┌──────────┬───────────┬─────────────────────────┐ │ │
│  │ │ 4B CRC32C│ 2B Length │ 1B Type                 │ │ │
│  │ │ (校验和) │ (数据长度) │ (Full/First/Middle/Last)│ │ │
│  │ └──────────┴───────────┴─────────────────────────┘ │ │
│  │                    Record Data                      │ │
│  └────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

- **块大小**: 32KB 固定块，便于管理和对齐
- **记录类型**: `Full`（完整记录）、`First`/`Middle`/`Last`（跨块分片记录）
- **CRC32C 校验**: 每条记录包含 CRC32C 校验和，检测数据损坏

#### 2.2 WAL Writer 写入器

- **自动边界处理**: 当记录跨越 32KB 块边界时，自动分片为 First/Middle/Last 类型
- **CRC 预计算表**: 预计算 CRC32C 类型表，加速校验和计算
- **追加写入**: 顺序追加，最大化写入吞吐

#### 2.3 WAL Reader 读取器

- **分片重组**: 自动重组 First/Middle/Last 分片记录
- **CRC 校验**: 读取时验证 CRC，检测损坏数据
- **Reporter 回调**: 发现损坏时调用回调函数通知上层
- **偏移同步处理**: 处理未完成的 fsync 产生的脏数据

#### 2.4 WritableFile 可写文件

- **64KB 缓冲**: 内置 64KB 写缓冲，减少系统调用
- **Append/Flush/Sync/Close**: 完整的写入生命周期管理
- **MANIFEST 特殊处理**: MANIFEST 文件写入后同步父目录，确保元数据持久化
- **工厂方法**: `NewWritableFile`（覆盖写）/ `NewAppendableFile`（追加写）

#### 2.5 SequentialFile 顺序读取文件

- **只读顺序访问**: 用于 WAL 回放和 MANIFEST 读取
- **Read/Skip 操作**: 读取和跳过指定字节数
- **ReadFileToString 辅助**: 将整个文件读入字符串

---

### 3. Table SSTable 模块

SSTable（Sorted String Table）是持久化数据的核心文件格式。

#### 3.1 SST 文件格式

```
┌─────────────────────────────────────────────────────────┐
│                    SST File Layout                      │
├─────────────────────────────────────────────────────────┤
│  [Data Block 0]                                          │
│  [Data Block 1]                                          │
│  ...                                                     │
│  [Data Block N]                                          │
│  [Meta Block (Filter Block)]                             │
│  [Meta Index Block]                                      │
│  [Index Block]                                           │
│  [Footer (48 bytes fixed)]                               │
│    ┌─────────────────────────────────────────────────┐   │
│    │ FilterBH | MetaIndexBH | IndexBH | Padding | Magic│ │
│    └─────────────────────────────────────────────────┘   │
│    Magic Number: 0xdb4775248b80fb57                      │
└─────────────────────────────────────────────────────────┘
```

- **BlockHandle**: varint 编码的偏移量 + 大小，紧凑存储
- **Footer**: 固定 48 字节，包含魔数 `0xdb4775248b80fb57` 用于文件完整性校验
- **ReadBlock**: 读取时自动 CRC 校验 + Snappy/Zstd 解压

#### 3.2 Block 数据块

SSTable 的基本存储单元：

- **前缀压缩**: Block 内相邻键共享前缀，仅存储差异后缀
- **Restart Point**: 每 `block_restart_interval`（默认 16）条记录设置一个完整键重启点
- **双向迭代**: 支持 forward/backward 遍历
- **二分搜索**: 基于 Restart Point 对键进行二分搜索，加速点查

#### 3.3 BlockBuilder 块构建器

- **前缀压缩优化**: 自动检测相邻键的共同前缀，减少冗余存储
- **重启点管理**: 定期插入完整键作为重启点，支持快速二分查找

#### 3.4 Table SSTable 表

- **两层迭代器**: Index Block → Data Block，透明遍历所有数据
- **Bloom Filter 优化**: 点查时先检查 Bloom Filter，快速跳过不存在的键
- **InternalGet**: 针对内部查询的优化路径，直接定位到目标 Block

#### 3.5 SSTBuilder SST 构建器

完整的 SST 文件构建流程：

1. 构建 Data Block（含前缀压缩）
2. 构建 Filter Block（每 2KB 数据段生成一个 Bloom Filter）
3. 构建 Meta Index Block
4. 构建 Index Block
5. 写入 Footer

- **压缩策略**: 支持 Snappy 和 Zstd，压缩率低于 12.5% 时自动回退为未压缩
- **Zstd 可配置级别**: 通过 config.xml 设置压缩级别

#### 3.6 Cache LRU 缓存

- **16 分片设计**: 减少锁竞争，每个分片独立互斥锁
- **哈希表 + 双向链表**: O(1) 查找 + LRU 淘汰
- **线程安全**: 分片级细粒度锁

#### 3.7 SSTCache SST 文件缓存

- **文件号映射**: 将 file_number 映射到 Table 对象
- **迭代器清理回调**: 迭代器销毁时自动释放关联资源
- **底层 LRU 缓存**: 复用通用 LRU 缓存实现

#### 3.8 FilterBlock 过滤器块

- **分段 Bloom Filter**: 每 2KB 数据段生成独立的 Bloom Filter
- **格式**: `[Filter 0][Filter 1]...[偏移数组][4B 偏移 + 1B log_base]`
- **高效构建/读取**: FilterBlockBuilder/FilterBlockReader 完整生命周期管理

#### 3.9 MergeIterator 合并迭代器

- **K 路归并**: 多路有序迭代器的归并排序
- **双向遍历**: 支持方向切换时的自动重定位
- **应用场景**: 多层级、多文件的有序数据合并

#### 3.10 TwoLevelIterator 两层迭代器

- **Index → Data 工厂**: 通过 Index Block 定位 Data Block，按需加载
- **透明遍历**: 上层无需关心底层 Block 边界

#### 3.11 RandomAccessFile 随机访问文件

- **pread 系统调用**: 线程安全的随机读取，无需维护文件偏移
- **FD Limiter 集成**: 控制打开的文件描述符数量

---

### 4. DB 数据库核心模块

#### 4.1 DB 公共 API

提供简洁的用户接口：

```cpp
DB::Open()          // 打开/创建数据库
Put(key, value)     // 写入键值对
Get(key, &value)    // 读取值
Delete(key)         // 删除键
Write(options, batch)  // 原子批量写入
NewIterator()       // 创建迭代器
GetSnapshot()       // 获取快照
ReleaseSnapshot()   // 释放快照
GetProperty(name)   // 获取属性
CompactRange(begin, end)  // 手动压缩
DestroyDB()         // 销毁数据库
RepairDB()          // 修复数据库
```

#### 4.2 DBImpl 核心实现

DeltaDB 的核心引擎，协调所有子系统：

- **写入队列**: 批量分组写入（Write Grouping），多个 WriteBatch 合并为一次 WAL 写入
- **WAL 写入**: 保证持久化，fsync 确保数据落盘
- **MemTable 管理**: 活跃 MemTable 和 Immutable MemTable 的切换
- **后台 Compaction**: 通过单线程池调度，触发条件和策略由 VersionSet 决定
- **快照管理**: 维护快照链表，保证一致性读
- **VersionSet 交互**: 版本链的更新和持久化
- **文件清理**: 异步清理不再被任何版本引用的旧 SST 文件

#### 4.3 MemTable 内存表

- **跳表后端**: 使用 SkipList 作为底层数据结构
- **内部键存储**: 存储带序列号和类型信息的内部键
- **Varint 长度前缀**: 键值对采用 varint 长度前缀格式
- **引用计数**: 安全的生命周期管理
- **操作**: `Add()`、`Get()`、`NewIterator()`

#### 4.4 WriteBatch 批量写入

- **原子操作**: 一批 Put/Delete 操作要么全部成功，要么全部失败
- **序列化格式**: 12 字节头（8B 序列号 + 4B 计数）+ varint 前缀字符串
- **WriteBatchInternal**: 提供底层操作接口
- **MemTableInserter**: 将 Batch 回放至 MemTable
- **WAL 合并写**: 多个并发 Write 合并为一次 WAL 写入，极大提升吞吐

#### 4.5 Snapshot 快照

- **双向循环链表**: `SnapshotList` 管理所有活跃快照
- **序列号**: 每个快照持有创建时的序列号，用于 MVCC 可见性判断
- **一致性读**: 快照确保读取操作只看到快照创建之前的数据

#### 4.6 DBIter 用户迭代器

- **内部迭代器包装**: 封装内部迭代器，提供用户视图
- **快照过滤**: 根据快照序列号过滤不可见的键版本
- **删除标记处理**: 自动跳过已删除的键
- **方向追踪**: 记录遍历方向，支持正向/反向切换
- **读取采样**: 记录读取样本，用于自动触发 Compaction

#### 4.7 VersionEdit 版本编辑

描述版本之间的变化：

- **记录内容**: 比较器名称、WAL 编号、下一个文件编号、最后序列号、Compaction 指针、新增/删除的 SST 文件
- **Tag 编码**: 基于 Tag 的二进制编码，紧凑高效

#### 4.8 VersionSet 版本管理

版本管理的核心系统：

- **Version**: 某个时间点的 SST 文件快照（跨 L0-L6 所有层）
- **VersionSet**: 版本链，通过 MANIFEST 文件持久化
- **Compaction 选择**: 基于文件大小（Size-based）或 Seek 次数（Seek-based）
- **Builder 高效应用**: `Builder` 结构高效地将编辑应用到当前版本
- **MANIFEST 回放**: 启动时重放 MANIFEST 恢复版本状态
- **Compaction 评分**: 计算每个层的 Compaction 分数，决定优先级

#### 4.9 Filename 文件命名

统一的文件命名规范：

| 文件类型 | 命名格式 | 说明 |
|---------|---------|------|
| WAL | `{dbname}/NNNNNN.wal` | 预写日志 |
| SST | `{dbname}/NNNNNN.sst` | 排序字符串表 |
| Temp | `{dbname}/NNNNNN.dbtmp` | 临时文件 |
| MANIFEST | `MANIFEST-NNNNNN` | 版本元数据 |
| CURRENT | `CURRENT` | 指向当前 MANIFEST |
| LOCK | `LOCK` | 数据库锁文件 |
| LOG | `LOG` | 运行日志 |

- **ParseFileName**: 解析文件名检测类型
- **SetCurrentFile**: 原子更新 CURRENT 文件

---

## 数据格式设计

### 内部键（Internal Key）

```
┌─────────────────────┬───────────────────┬──────────────┐
│      user_key       │  sequence (56bit) │ type (8bit)  │
│   (变长，用户定义)   │   (MVCC 版本号)    │ (ValueType) │
└─────────────────────┴───────────────────┴──────────────┘
```

- **ValueType**: `kTypeValue` (0x0) 表示有效值，`kTypeDeletion` (0x1) 表示删除标记
- **比较规则**: user_key 升序 → sequence 降序（确保相同 key 的最新版本优先被扫描到）

### WriteBatch 序列化格式

```
┌───────────────────┬─────────────────────────────────────┐
│  Header (12 bytes)│          Record Body                │
│ ┌────────┬───────┐│ ┌─────────────────────────────────┐ │
│ │ Seq(8B)│Cnt(4B)│││ [tag][varint_len][data]... 重复  │ │
│ └────────┴───────┘│└─────────────────────────────────┘ │
└───────────────────┴─────────────────────────────────────┘
```

---

## 写入路径

```
用户 Put/Delete
       │
       ▼
  ┌─────────┐
  │WriteBatch│ ← 多个 Write 合并为一个 Batch
  └────┬─────┘
       │
       ▼
  ┌──────────┐
  │  WAL 写入 │ ← 顺序追加到 WAL，fsync 保障持久化
  └────┬─────┘
       │
       ▼
  ┌───────────┐
  │ MemTable  │ ← 写入活跃 MemTable（跳表结构）
  └─────┬─────┘
        │
        │ MemTable 满 (write_buffer_size)
        ▼
  ┌──────────────┐
  │ Immutable MT │ ← 切换为只读，触发后台 Compaction
  └──────┬───────┘
         │
         ▼
  ┌──────────────┐
  │  Flush to L0 │ ← 生成 L0 层 SSTable 文件
  └──────────────┘
```

---

## 读取路径

```
用户 Get(key)
     │
     ▼
┌─────────────┐  未找到？   ┌──────────────┐
│  MemTable   │ ──────────▶ │ Immutable MT │
└─────────────┘             └──────┬───────┘
                                   │
                            未找到？│
                                   ▼
                          ┌────────────────┐
                          │  L0 SST files  │ ← 可能有重叠，逐个检查
                          └───────┬────────┘
                                  │
                           未找到？│
                                  ▼
                          ┌────────────────┐
                          │ L1 → L6 SST    │ ← Bloom Filter 快速跳过
                          │ (Bloom Filter) │   不存在的文件
                          └────────────────┘
```

**Bloom Filter 优化**: 在读取 SSTable 之前，先检查对应文件的 Bloom Filter。如果 Bloom Filter 返回 negative，则直接跳过该文件，避免无效磁盘 I/O。

---

## Compaction 机制

```
  MemTable (满)
       │ flush
       ▼
  ┌─────────┐
  │  L0     │ ← 文件可能有键重叠
  │ (4 文件) │ ← L0_compaction_trigger = 4 触发
  └────┬────┘
       │ compaction
       ▼
  ┌─────────┐
  │  L1     │ ← 文件无重叠，按序排列
  │         │
  └────┬────┘
       │
       ▼
  ┌─────────┐
  │  L2     │
  └────┬────┘
       │
       ...
       │
       ▼
  ┌─────────┐
  │  L6     │ ← 最终层，包含最多数据
  └─────────┘
```

**触发策略**:
- **Size-based**: 某层文件数/大小超过阈值时触发 Compaction
- **Seek-based**: 某文件 Seek 次数过多（说明 Bloom Filter 误判率高），触发 Compaction 清理

**Compaction 过程**:
1. 选择待 Compaction 的输入文件
2. 与下一层有重叠的文件合并
3. 生成新的 SSTable 文件
4. 更新 VersionSet，记录新增/删除的文件
5. 持久化到 MANIFEST
6. 异步清理旧文件

---

## 构建与运行

### 依赖

| 库 | 用途 |
|----|------|
| **crc32c** | CRC32C 校验和计算 |
| **ZLIB** | 数据压缩 |
| **GTest** | 单元测试 |
| **TinyXML** | XML 配置解析 |
| **Snappy** | 快速压缩（可选） |
| **Zstd** | 高压缩比压缩（可选） |

### 构建

```bash
# 一键构建（清理 + CMake + 编译）
./to_build.sh

# 手动构建
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

构建产物：
- **可执行文件**: `build/bin/deltadb`
- **库文件**: `build/lib/` 下的静态库

### 运行 REPL

```bash
./build/bin/deltadb              # 默认数据库路径 ./output/delta
./build/bin/deltadb --db /path   # 指定数据库路径
```

### REPL 命令

| 命令 | 说明 |
|------|------|
| `put <key> <value>` | 写入键值对 |
| `get <key>` | 查询值 |
| `del <key>` | 删除键 |
| `scan [begin] [end]` | 范围扫描 |
| `compact [begin] [end]` | 手动 Compaction |
| `stats` | 查看统计信息 |
| `dbdir` | 查看数据库目录 |
| `help` | 帮助 |
| `quit` | 退出 |

---

## 配置说明

配置文件位于 `conf/config.xml`，主要配置项：

### 日志配置

```xml
<log>
    <path>./output/log</path>       <!-- 日志目录 -->
    <prefix>delta</prefix>           <!-- 日志文件名前缀 -->
    <max_file_size>5242880</max_file_size>  <!-- 单文件最大 5MB -->
    <level>DEBUG</level>             <!-- 日志级别 -->
    <sync_interval>500</sync_interval>      <!-- 同步间隔 500ms -->
</log>
```

### WAL 配置

```xml
<wal>
    <path>./output/wal</path>        <!-- WAL 目录 -->
    <base_name>WAL</base_name>       <!-- WAL 文件名前缀 -->
    <max_file_size>10485760</max_file_size> <!-- 单文件最大 10MB -->
</wal>
```

### 数据库配置

```xml
<db>
    <num_levels>7</num_levels>                    <!-- LSM 层数 -->
    <l0_compaction_trigger>4</l0_compaction_trigger>  <!-- L0 触发 Compaction 文件数 -->
    <l0_slowdown_writes_trigger>8</l0_slowdown_writes_trigger>  <!-- L0 写入减速阈值 -->
    <l0_stop_writes_trigger>12</l0_stop_writes_trigger>  <!-- L0 写入停止阈值 -->
    <write_buffer_size>4194304</write_buffer_size>  <!-- MemTable 大小 4MB -->
    <block_size>4096</block_size>                   <!-- SST Block 大小 4KB -->
    <block_restart_interval>16</block_restart_interval>  <!-- Block 重启点间隔 -->
    <max_sst_size>2097152</max_sst_size>            <!-- 最大 SST 文件大小 2MB -->
    <zstd_compression_level>1</zstd_compression_level>  <!-- Zstd 压缩级别 -->
    <fd_limiter>50</fd_limiter>                     <!-- 文件描述符限制 -->
</db>
```

---

## 技术亮点

### 1. 写入队列与批量分组（Write Grouping）

多个并发写入请求被合并为一个 WriteBatch，一次性写入 WAL 和 MemTable。这极大减少了 fsync 的次数，将随机写转为顺序追加，显著提升了写入吞吐量。

### 2. 无锁读跳表

MemTable 的 SkipList 采用原子指针发布机制，读取端完全无锁。写操作由外部互斥锁保护，读写互不阻塞，在读多写少的场景下性能优异。

### 3. 两层 SSTable 迭代器

Index Block 指向 Data Block，通过 TwoLevelIterator 透明遍历。上层无需关心 Block 边界，按需加载 Data Block 到缓存，有效利用内存。

### 4. Bloom Filter 优化

每 2KB 数据段生成独立的 Bloom Filter，点查时先检查 Bloom Filter。如果确定键不存在，直接跳过该 SSTable 文件，避免无效的磁盘 I/O。在高误判容忍场景下（如 1%），可以过滤掉 99% 的无效读取。

### 5. 前缀压缩存储

SSTable Block 内相邻键共享前缀，仅存储差异后缀。每 16 条记录设置一个 Restart Point 保证二分搜索的正确性。对于有序写入的场景（如时间序列），前缀压缩率极高。

### 6. 分片 LRU 缓存

16 个独立的缓存分片，每个分片有自己的互斥锁、哈希表和 LRU 链表。相比全局锁的缓存，分片设计大幅减少了锁竞争，提升了并发读性能。

### 7. MVCC 快照隔离

56 位序列号 + 双向循环链表管理快照。每个快照持有创建时的序列号，读取时只看到序列号小于等于快照序列号的键版本。这保证了在 Compaction 和并发读写场景下的一致性读。

### 8. 自适应压缩

SSTable 构建时尝试 Snappy 或 Zstd 压缩，如果压缩后大小 reduction < 12.5%，则回退到未压缩格式。这避免了在不可压缩数据上的 CPU 浪费。

### 9. WAL 分片记录

当记录跨越 32KB 块边界时，自动分片为 First/Middle/Last 类型。读取端自动重组，确保数据完整性同时避免了块浪费。

### 10. MANIFEST 原子更新

MANIFEST 文件写入后同步父目录（fsync + fdatasync），确保元数据变更持久化到存储设备。即使发生崩溃，也能通过 MANIFEST 回放恢复版本状态。

### 11. 异步日志系统

独立的日志线程缓冲写入请求，按文件大小自动轮转。主路径的日志写入不会阻塞核心读写逻辑，日志级别从 DEBUG 到 ERROR 灵活可控。

### 12. 文件描述符限流

通过 `Limiter` 原子计数器控制打开的文件描述符数量。当 SSTable 文件数量很多时，不会超出系统限制，保障了数据库的稳定性。

---

## 许可证

本项目为学习/研究用途的嵌入式 LSM-Tree 存储引擎实现。
