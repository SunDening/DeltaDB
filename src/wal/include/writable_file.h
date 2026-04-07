#pragma once

#include <assert.h>
#include <fcntl.h>
#include <string_view>

#include "status.h"

namespace delta {

constexpr const size_t kWritableFileBufferSize = 65536;
#ifndef DELTA_K_OPEN_BASE_FLAGS
#    define DELTA_K_OPEN_BASE_FLAGS
#    if defined(HAVE_O_CLOEXEC)
constexpr const int kOpenBaseFlags = O_CLOEXEC;
#    else
constexpr const int kOpenBaseFlags = 0;
#    endif
#endif

/**
 * 追加写入的文件接口
 */
class WritableFile {
   public:
    WritableFile(std::string filename, int fd)
        : pos_(0),
          fd_(fd),
          is_manifest_(IsManifest(filename)),
          filename_(std::move(filename)),
          dirname_(Dirname(filename)) {}

    virtual ~WritableFile() {
        if (fd_ >= 0) {
            Close();  // 析构时自动关闭
        }
    }

    /**
     *
        用户数据
            ↓ Append
        内存缓冲区 [64KB]
            ↓ Flush
        Linux 页缓存
            ↓ Sync
        物理磁盘
     */

    // 追加数据到缓冲区
    Status Append(const std::string_view data);

    // 刷新缓冲区到操作系统缓存
    Status Flush();

    // 同步到磁盘（持久化）
    virtual Status Sync();

    // 关闭文件
    Status Close();

   private:
    char buf_[kWritableFileBufferSize];  // 64KB 缓冲区
    size_t pos_;                         // 缓冲区写入位置
    int fd_;                             // linux 文件描述符
    const bool is_manifest_;             // Manifest 文件特殊处理
    const std::string filename_;
    const std::string dirname_;

    static std::string_view Basename(const std::string& filename);

    static std::string Dirname(const std::string& filename);

    static bool IsManifest(const std::string& filename);

    Status FlushBuffer();

    Status WriteUnbuffered(const char* data, size_t size);

    Status SyncDirIfManifest();

    static Status SyncFd(int fd, const std::string& fd_path);
};

inline Status NewWritableFile(const std::string& filename, WritableFile** result) {
    int fd = ::open(filename.c_str(), O_TRUNC | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
    if (fd < 0) {
        *result = nullptr;
        return PosixError(filename, errno);
    }

    *result = new WritableFile(filename, fd);
    return Status::OK();
}

inline Status NewAppendableFile(const std::string& filename, WritableFile** result) {
    int fd = ::open(filename.c_str(), O_APPEND | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
    if (fd < 0) {
        *result = nullptr;
        return PosixError(filename, errno);
    }

    *result = new WritableFile(filename, fd);
    return Status::OK();
}

}  // namespace delta