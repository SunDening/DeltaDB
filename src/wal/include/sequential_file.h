#pragma once

#include <fcntl.h>
#include <string>

#include "status.h"

namespace delta {

#ifndef DELTA_K_OPEN_BASE_FLAGS
#    define DELTA_K_OPEN_BASE_FLAGS
#    if defined(HAVE_O_CLOEXEC)
constexpr const int kOpenBaseFlags = O_CLOEXEC;
#    else
constexpr const int kOpenBaseFlags = 0;
#    endif  // defined(HAVE_O_CLOEXEC)
#endif

/**
 * SequentialFile 是用于 顺序读取 文件的接口
 * 核心特点：
 *  1. 单线前进：只能顺序读取，不支持随机跳转到任意位置
 *  2. 简洁高效：只有 Read 和 Skip 两个方法，专注于顺序访问场景
 *  3. 线程不安全：需要外部保护
 * 主要用于需要顺序读取的场景，最典型的就是：
 *  WAL 日志文件：wal_reader 使用 SequentialFile 顺序读取预写日志恢复数据
 *  MANIFEST 文件：版本控制日志也需要顺序读取
 */
class SequentialFile {
   private:
    const int fd_;                // 文件描述符
    const std::string filename_;  // 文件名（用于错误信息）

   public:
    SequentialFile(std::string filename, int fd);
    ~SequentialFile();

    /**
     * 从文件中读取最多 n 个字节
     * scratch[0, n-1] 可能被写入
     * result 指向读取的数据
     */
    Status Read(size_t n, std::string_view* result, char* scratch);

    /**
     * 跳过文件中的 n 个字节
     * 纯指针操作，保证不比读取相同数据慢，可能更快
     */
    Status Skip(uint64_t n);
};

inline Status NewSequentialFile(const std::string& filename, SequentialFile** result) {
    int fd = ::open(filename.c_str(), O_RDONLY | kOpenBaseFlags);
    if (fd < 0) {
        *result = nullptr;
        return PosixError(filename, errno);
    }

    *result = new SequentialFile(filename, fd);
    return Status::OK();
}

inline Status ReadFileToString(const std::string& fname, std::string* data) {
    data->clear();
    SequentialFile* file;
    Status s = NewSequentialFile(fname, &file);
    if (!s.ok()) {
        return s;
    }
    static const int kBufferSize = 8192;
    char* space = new char[kBufferSize];
    while (true) {
        std::string_view fragment;
        s = file->Read(kBufferSize, &fragment, space);
        if (!s.ok()) {
            break;
        }
        data->append(fragment.data(), fragment.size());
        if (fragment.empty()) {
            break;
        }
    }
    delete[] space;
    delete file;
    return s;
}

}  // namespace delta
