#pragma once

#include <assert.h>

#include <deltadb/utils/config.h>
#include <deltadb/utils/status.h>
#include <deltadb/utils/util.h>

namespace delta {

extern delta::Config::ptr gDBConfig;

// Common flags defined for all posix open operations
#if defined(HAVE_O_CLOEXEC)
constexpr const int kOpenBaseFlags = O_CLOEXEC;
#else
constexpr const int kOpenBaseFlags = 0;
#endif  // defined(HAVE_O_CLOEXEC)

/**
 * 是一个用于随机读取文件内容的抽象类
 *
 * 设计目标：
 *  1. 支持多线程并发读取 (线程安全)
 *  2. 使用 pread() 避免 lseek() 的竞态条件
 *  3. 通过 fd_limiter 控制打开的文件描述符数量，防止资源耗尽
 */
class RandomAccessFile {
   private:
    const bool has_permanent_fd_;  // true：持有永久 fd；false：每次 READ 临时打开
    const int fd_;                 // 永久文件描述符，-1表示无
    Limiter* const fd_limiter_;    // FD 限额控制器（必须比本对象生命周期长）
    const std::string filename_;   // 文件路径（用于临时打开时获取 fd）

   public:
    RandomAccessFile(std::string filename, int fd, Limiter* fd_limiter)
        : has_permanent_fd_(fd_limiter->Acquire()),
          fd_(has_permanent_fd_ ? fd : -1),
          fd_limiter_(fd_limiter),
          filename_(std::move(filename)) {
        if (!has_permanent_fd_) {
            assert(fd_ == -1);
            ::close(fd);
        }
    }

    ~RandomAccessFile() {
        if (has_permanent_fd_) {
            assert(fd_ != -1);
            ::close(fd_);            // 关闭文件描述符
            fd_limiter_->Release();  // 归还配额
        }
    }

    /**
     * @brief 从指定偏移量读取数据
     * @param offset：读取起始位置
     * @param n：最多读取的字节数
     * @param result：输出参数，指向实际读取的数据
     * @param scratch：用户提供的缓冲区
     */
    Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const {
        // 确定使用的文件描述符
        int fd = fd_;
        if (!has_permanent_fd_) {
            // 无永久 FD，每次 Read 临时打开文件
            fd = ::open(filename_.c_str(), O_RDONLY | kOpenBaseFlags);
            if (fd < 0) {
                return PosixError(filename_, errno);
            }
        }

        assert(fd != -1);

        // 执行预读
        Status status;
        ssize_t read_size = ::pread(fd, scratch, n, static_cast<off_t>(offset));

        // 设置返回结果
        *result = std::string_view(scratch, (read_size < 0) ? 0 : read_size);
        if (read_size < 0) {
            status = PosixError(filename_, errno);
        }

        if (!has_permanent_fd_) {
            // 无永久 FD，关闭临时打开的 fd
            assert(fd != fd_);
            ::close(fd);
        }
        return status;
    }
};

inline static Status NewRandomAccessFile(const std::string& filename, RandomAccessFile** result) {
    *result = nullptr;
    int fd = ::open(filename.c_str(), O_RDONLY | kOpenBaseFlags);
    if (fd < 0) {
        return PosixError(filename, errno);
    }

    *result = new RandomAccessFile(filename, fd, &gDBConfig->fd_limiter);
    // 注意：这里不需要 close(fd)，因为 RandomAccessFile 会接管 fd 的所有权
    return Status::OK();
}
}  // namespace delta