#include <cstdint>
#include <format>

#include <deltadb/db/filename.h>
#include <deltadb/utils/dbformat.h>
#include <deltadb/utils/util.h>

namespace delta {

// ============================================================================
// 前置声明：内部辅助函数
// ============================================================================
/**
 * @brief 将数据写入命名文件并同步到磁盘
 *
 * @param data：要写入的数据
 * @param fname：目标文件名
 */
// Status WriteStringToFileSync(const std::string_view& data, const std::string& fname);

// ============================================================================
// 核心工具函数：生成标准文件名
// ============================================================================
/**
 * @brief 生成标准文件名格式. 格式：{dbname}/{06d 编号}.{后缀}
 *
 * @param dbname：数据库目录路径
 * @param number：文件编号（6位数字）
 * @param suffix：文件后缀（扩展名）
 * @return 完整文件名
 */
static std::string MakeFileName(const std::string& dbname, uint64_t number, const char* suffix) {
    return std::format("{}/{:06}.{}", dbname, number, suffix);
}

// ============================================================================
// 公开 API：文件名生成函数
// ============================================================================

/**
 * @brief 生成 WAL 日志文件名
 *
 * @param dbname：数据库路径
 * @param number：文件编号
 * @return WAL 文件名
 */
std::string WalFileName(const std::string& dbname, uint64_t number) {
    assert(number > 0);
    return MakeFileName(dbname, number, "wal");
}

/**
 * @brief 生成 SST 表文件名
 *
 * @param dbname：数据库路径
 * @param number：文件编号
 * @return SST 文件名
 */
std::string SSTFileName(const std::string& dbname, uint64_t number) {
    assert(number > 0);
    return MakeFileName(dbname, number, "sst");
}

/**
 * @brief 生成 MANIFEST 描述文件名
 *
 * @param dbname：数据库路径
 * @param number：文件编号
 * @return MANIFEST 文件名
 */
std::string ManifestFileName(const std::string& dbname, uint64_t number) {
    assert(number > 0);
    return std::format("{}/MANIFEST-{:06}", dbname, number);
}

/**
 * @brief 生成 CURRENT 文件名
 *
 * @param dbname：数据库路径
 * @return CURRENT 文件名
 */
std::string CurrentFileName(const std::string& dbname) { return dbname + "/CURRENT"; }

/**
 * @brief 生成 LOCK 锁文件名
 *
 * @param dbname：数据库路径
 * @return LOCK 文件名
 */
std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

/**
 * @brief 生成临时文件名
 *
 * @param dbname：数据库路径
 * @param number：文件编号
 * @return 临时文件名
 */
std::string TempFileName(const std::string& dbname, uint64_t number) {
    assert(number > 0);
    return MakeFileName(dbname, number, "dbtmp");
}

/**
 * @brief 生成当前信息日志文件名
 *
 * @param dbname：数据库路径
 * @return 信息日志文件名
 */
std::string InfoLogFileName(const std::string& dbname) { return dbname + "/LOG"; }

/**
 * @brief 生成旧信息日志文件名
 *
 * @param dbname：数据库路径
 * @return 旧信息日志文件名
 */
std::string OldInfoLogFileName(const std::string& dbname) { return dbname + "/LOG.old"; }

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
bool ParseFileName(const std::string& filename, uint64_t* number, FileType* type) {
    std::string_view rest(filename);

    // 特殊文件（无编号）
    if (rest == "CURRENT") {
        *number = 0;
        *type = kCurrentFile;
    } else if (rest == "LOCK") {
        *number = 0;
        *type = kDBLockFile;
    } else if (rest == "LOG" || rest == "LOG.old") {
        *number = 0;
        *type = kInfoLogFile;
    } else if (rest.starts_with("MANIFEST-")) {
        // MANIFEST 文件
        rest.remove_prefix(strlen("MANIFEST-"));
        uint64_t num;
        // 解析十进制数字
        if (!ConsumeDecimalNumber(&rest, &num)) {
            return false;
        }
        if (!rest.empty()) {
            return false;
        }
        *type = kManifestFile;
        *number = num;
    } else {
        // 数字编号文件
        uint64_t num;
        if (!ConsumeDecimalNumber(&rest, &num)) {
            return false;
        }
        std::string_view suffix = rest;
        // 根据后缀判断文件类型
        if (suffix == std::string_view(".wal")) {
            *type = kWalFile;
        } else if (suffix == std::string_view(".sst")) {
            *type = kSSTFile;
        } else if (suffix == std::string_view(".dbtmp")) {
            *type = kTempFile;
        } else {
            return false;
        }
        *number = num;
    }
    return true;
}

// ============================================================================
// CURRENT 文件更新
// ============================================================================

/**
 * @brief 更新 CURRENT 文件，指向指定的 MANIFEST 文件
 *
 * @param dbname：数据库路径
 * @param manifest_number：MANIFEST 文件编号
 *
 * 操作流程：
 *  1. 生成 MANIFEST 完整路径
 *  2. 去掉 dbname/ 前缀，只保留文件名
 *  3. 写入临时文件（内容："MANIFEST-{编号}\n"）
 *  4. 原子重命名为 CURRENT
 *  5. 失败时清理临时文件
 */
Status SetCurrentFile(const std::string& dbname, uint64_t manifest_number) {
    // 生成 MANIFEST 完整路径
    std::string manifest = ManifestFileName(dbname, manifest_number);

    std::string_view contents = manifest;

    // 去掉 dbname/ 前缀
    assert(contents.starts_with(dbname + "/"));
    contents.remove_prefix(dbname.size() + 1);

    // 生成临时文件名
    std::string tmp = TempFileName(dbname, manifest_number);

    // 写入临时文件   临时文件内容："MANIFEST-000001\n"
    Status s = WriteStringToFileSync(std::format("{}\n", contents.data()), tmp);

    // 原子重命名为 CURRENT
    if (s.ok()) {
        s = RenameFile(tmp, CurrentFileName(dbname));
    }
    if (!s.ok()) {
        RemoveFile(tmp);
    }
    return s;
}

}  // namespace delta