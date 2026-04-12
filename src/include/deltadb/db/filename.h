#pragma once

#include <cstdint>

#include <deltadb/utils/status.h>

namespace delta {

/**
 * 定义数据库目录中所有文件的命名规则，确保：
 *  - 文件类型可通过文件名识别
 *  - 文件编号唯一且有序
 *  - 支持文件解析和恢复
 */

//  文件类型
enum FileType {
    kWalFile,       // WAL 日志文件 	{number}.wal
    kDBLockFile,    // 数据库锁文件  LOCK
    kSSTFile,       // SST 表文件  {number}.sst  {number}.ldb
    kManifestFile,  // MANIFEST 描述文件  MANIFEST-{number}
    kCurrentFile,   // CURRENT 当前文件  CURRENT 指向当前 MANIFEST
    kTempFile,      // 临时文件  {number}.tmp
    kInfoLogFile    // 信息日志文件（LOG/LOG.old）
};

/**
 * @brief 生成 WAL 日志文件名
 *
 * @param dbname：数据库路径
 * @param number：文件编号
 * @return WAL 文件名
 */
std::string WalFileName(const std::string& dbname, uint64_t number);

/**
 * @brief 生成 SST 表文件名
 *
 * @param dbname：数据库路径
 * @param number：文件编号
 * @return SST 文件名
 */
std::string SSTFileName(const std::string& dbname, uint64_t number);

/**
 * @brief 生成 MANIFEST 描述文件名
 *
 * @param dbname：数据库路径
 * @param number：文件编号
 * @return MANIFEST 文件名
 */
std::string ManifestFileName(const std::string& dbname, uint64_t number);

/**
 * @brief 生成 CURRENT 文件名
 *
 * @param dbname：数据库路径
 * @return CURRENT 文件名
 */
std::string CurrentFileName(const std::string& dbname);

/**
 * @brief 生成 LOCK 锁文件名
 *
 * @param dbname：数据库路径
 * @return LOCK 文件名
 */
std::string LockFileName(const std::string& dbname);

/**
 * @brief 生成临时文件名
 *
 * @param dbname：数据库路径
 * @param number：文件编号
 * @return 临时文件名
 */
std::string TempFileName(const std::string& dbname, uint64_t number);

/**
 * @brief 生成当前信息日志文件名
 *
 * @param dbname：数据库路径
 * @return 信息日志文件名
 */
std::string InfoLogFileName(const std::string& dbname);

/**
 * @brief 生成旧信息日志文件名
 *
 * @param dbname：数据库路径
 * @return 旧信息日志文件名
 */
std::string OldInfoLogFileName(const std::string& dbname);

/**
 * @brief 解析文件名
 *
 * @param filename：文件名（不含路径）
 * @param number：输出，文件编号
 * @param type：输出，文件类型
 * @return bool：解析是否成功
 *
 * 解析规则：
 * - 000002.log    → number=2, type=kLogFile
 * - 000003.sst    → number=3, type=kTableFile
 * - 000003.ldb    → number=3, type=kTableFile
 * - MANIFEST-000001 → number=1, type=kDescriptorFile
 * - 000010.tmp    → number=10, type=kTempFile
 * - CURRENT/LOCK/LOG → number=0, type=对应类型
 * - 其他 → 返回 false
 */
bool ParseFileName(const std::string& filename, uint64_t* number, FileType* type);

/**
 * @brief 更新 CURRENT 文件，指向指定的 MANIFEST
 *
 * @param dbname：数据库路径
 * @param manifest_number：MANIFEST 文件编号
 *
 * 操作：
 *  1. 写入临时文件 {dbname}/CURRENT.tmp
 *  2. 内容为 "MANIFEST-{number}\n"
 *  3. 原子重命名为 CURRENT
 *
 * 用途：
 *  - 数据库启动时读取 CURRENT 找到当前 MANIFEST
 *  - 新的 MANIFEST 创建后更新 CURRENT
 */
Status SetCurrentFile(const std::string& dbname, uint64_t manifest_number);

}  // namespace delta