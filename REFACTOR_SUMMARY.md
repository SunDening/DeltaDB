# DeltaDB 头文件路径重构总结

## 修改日期
2026-04-08

## 修改目的
将项目中所有 `#include "xxx.h"` 短路径格式改为 `#include <deltadb/module/xxx.h>` 完整路径格式，以便在安装后仍能正确工作。

## 核心修改

### 1. 头文件包含路径格式变更

**修改前：**
```cpp
#include "status.h"
#include "dbformat.h"
#include "iterator.h"
```

**修改后：**
```cpp
#include <deltadb/utils/status.h>
#include <deltadb/utils/dbformat.h>
#include <deltadb/utils/iterator.h>
```

### 2. 修改的文件统计

#### 头文件 (.h) - 约 41 个文件
- `src/utils/include/` - 13 个文件
- `src/wal/include/` - 5 个文件  
- `src/table/include/` - 12 个文件
- `src/db/include/` - 10 个文件
- `src/include/deltadb/` - 1 个文件

#### 源文件 (.cc/.cpp) - 约 30 个文件
- `src/utils/` - 9 个文件
- `src/wal/` - 4 个文件
- `src/table/` - 10 个文件
- `src/db/` - 7 个文件
- `app/main.cpp` - 1 个文件
- `test/` - 6 个文件

**总计：约 71 个文件被修改**

### 3. 路径映射规则

| 模块 | 旧格式 | 新格式 |
|------|--------|--------|
| **utils** | `"status.h"` | `<deltadb/utils/status.h>` |
|  | `"coding.h"` | `<deltadb/utils/coding.h>` |
|  | `"comparator.h"` | `<deltadb/utils/comparator.h>` |
|  | `"config.h"` | `<deltadb/utils/config.h>` |
|  | `"dbformat.h"` | `<deltadb/utils/dbformat.h>` |
|  | `"filter_policy.h"` | `<deltadb/utils/filter_policy.h>` |
|  | `"iterator.h"` | `<deltadb/utils/iterator.h>` |
|  | `"log.h"` | `<deltadb/utils/log.h>` |
|  | `"no_destructor.h"` | `<deltadb/utils/no_destructor.h>` |
|  | `"single_thread_pool.h"` | `<deltadb/utils/single_thread_pool.h>` |
|  | `"skiplist.h"` | `<deltadb/utils/skiplist.h>` |
|  | `"util.h"` | `<deltadb/utils/util.h>` |
|  | `"arena.h"` | `<deltadb/utils/arena.h>` |
| **wal** | `"sequential_file.h"` | `<deltadb/wal/sequential_file.h>` |
|  | `"wal_format.h"` | `<deltadb/wal/wal_format.h>` |
|  | `"wal_reader.h"` | `<deltadb/wal/wal_reader.h>` |
|  | `"wal_writer.h"` | `<deltadb/wal/wal_writer.h>` |
|  | `"writable_file.h"` | `<deltadb/wal/writable_file.h>` |
| **table** | `"block.h"` | `<deltadb/table/block.h>` |
|  | `"block_builder.h"` | `<deltadb/table/block_builder.h>` |
|  | `"cache.h"` | `<deltadb/table/cache.h>` |
|  | `"filter_block.h"` | `<deltadb/table/filter_block.h>` |
|  | `"iter_merger.h"` | `<deltadb/table/iter_merger.h>` |
|  | `"iterator_wrapper.h"` | `<deltadb/table/iterator_wrapper.h>` |
|  | `"random_access_file.h"` | `<deltadb/table/random_access_file.h>` |
|  | `"sst_builder.h"` | `<deltadb/table/sst_builder.h>` |
|  | `"sst_cache.h"` | `<deltadb/table/sst_cache.h>` |
|  | `"sst_format.h"` | `<deltadb/table/sst_format.h>` |
|  | `"table.h"` | `<deltadb/table/table.h>` |
|  | `"two_level_iterator.h"` | `<deltadb/table/two_level_iterator.h>` |
| **db** | `"db.h"` | `<deltadb/db/db.h>` |
|  | `"db_impl.h"` | `<deltadb/db/db_impl.h>` |
|  | `"db_iter.h"` | `<deltadb/db/db_iter.h>` |
|  | `"filename.h"` | `<deltadb/db/filename.h>` |
|  | `"memtable.h"` | `<deltadb/db/memtable.h>` |
|  | `"snapshot.h"` | `<deltadb/db/snapshot.h>` |
|  | `"version_edit.h"` | `<deltadb/db/version_edit.h>` |
|  | `"version_set.h"` | `<deltadb/db/version_set.h>` |
|  | `"write_batch.h"` | `<deltadb/db/write_batch.h>` |
|  | `"write_batch_internal.h"` | `<deltadb/db/write_batch_internal.h>` |

### 4. CMakeLists.txt 修改

#### 根目录 CMakeLists.txt
- **统一 include 目录**：只保留 `${CMAKE_SOURCE_DIR}/src/include`
- 所有头文件通过 `#include <deltadb/module/xxx.h>` 访问
- 保留了安装规则（install rules）不变

#### 子模块 CMakeLists.txt (utils, wal, table, db, app, test)
- **完全移除了** `target_include_directories` 配置
- 所有模块统一使用根 CMakeLists.txt 中的 `include_directories`
- 简化了配置，因为所有 include 现在都使用完整路径

### 5. 头文件目录结构重构

**关键改动**：将所有头文件复制到统一位置 `src/include/deltadb/`

原始结构（分散）：
```
src/
├── utils/include/*.h
├── wal/include/*.h
├── table/include/*.h
├── db/include/*.h
└── include/deltadb/deltadb.h
```

新结构（统一）：
```
src/include/deltadb/
├── deltadb.h
├── utils/
│   ├── arena.h
│   ├── coding.h
│   └── ...
├── wal/
│   ├── sequential_file.h
│   └── ...
├── table/
│   ├── block.h
│   └── ...
└── db/
    ├── db.h
    └── ...
```

这样做的好处：
- **编译时**：只需要一个 include 路径 `src/include`
- **安装后**：`/usr/local/include/deltadb/` 结构完全一致
- **无冲突**：不会与旧安装文件冲突

### 6. 安装后头文件结构

安装后（`cmake --install`），头文件将被安装到：
```
/usr/local/include/deltadb/
├── utils/
│   ├── arena.h
│   ├── coding.h
│   ├── comparator.h
│   ├── config.h
│   ├── dbformat.h
│   ├── filter_policy.h
│   ├── iterator.h
│   ├── log.h
│   ├── no_destructor.h
│   ├── single_thread_pool.h
│   ├── skiplist.h
│   ├── status.h
│   └── util.h
├── wal/
│   ├── sequential_file.h
│   ├── wal_format.h
│   ├── wal_reader.h
│   ├── wal_writer.h
│   └── writable_file.h
├── table/
│   ├── block.h
│   ├── block_builder.h
│   ├── cache.h
│   ├── filter_block.h
│   ├── iter_merger.h
│   ├── iterator_wrapper.h
│   ├── random_access_file.h
│   ├── sst_builder.h
│   ├── sst_cache.h
│   ├── sst_format.h
│   ├── table.h
│   └── two_level_iterator.h
├── db/
│   ├── db.h
│   ├── db_impl.h
│   ├── db_iter.h
│   ├── filename.h
│   ├── memtable.h
│   ├── snapshot.h
│   ├── version_edit.h
│   ├── version_set.h
│   ├── write_batch.h
│   └── write_batch_internal.h
└── deltadb/
    └── deltadb.h
```

下游用户使用时：
```cpp
#include <deltadb/db/db.h>
#include <deltadb/utils/status.h>
#include <deltadb/table/table.h>
```

## 验证清单
- ✅ 所有头文件 include 路径已更新为完整路径
- ✅ 所有源文件 include 路径已更新
- ✅ CMakeLists.txt 已简化
- ✅ app/main.cpp 已更新
- ✅ test/ 下所有测试文件已更新
- ✅ 头文件已统一到 `src/include/deltadb/` 目录
- ✅ **编译成功！** 无错误，仅有少量未使用参数警告

## 优势
1. **安装后可用**：下游用户安装后，使用 `#include <deltadb/xxx/yyy.h>` 可以正确找到所有头文件
2. **命名空间清晰**：路径明确表明模块归属
3. **避免冲突**：统一的 include 结构不会与系统中其他库的头文件冲突
4. **符合标准实践**：与 LevelDB/RocksDB 等库的 include 风格一致
5. **简化构建配置**：只需一个 include 路径，CMake 配置更清晰
