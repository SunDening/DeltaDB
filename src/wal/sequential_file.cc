#include <deltadb/utils/log.h>
#include <deltadb/wal/sequential_file.h>

namespace delta {

SequentialFile::SequentialFile(std::string filename, int fd) : fd_(fd), filename_(std::move(filename)) {
    InfoLog << std::format("SequentialFile {} init complate", filename_);
}

SequentialFile::~SequentialFile() { close(fd_); }

/**
 * 顺序读取
 * 读取一个块的内容，存入 result（buffer_）
 * scratch 仅仅作为读取时的临时缓冲区
 */
Status SequentialFile::Read(size_t n, std::string_view* result, char* scratch) {
    Status status;
    // while 不是多次读取，仅仅为重试用
    while (true) {
        ssize_t read_size = read(fd_, scratch, n);  // 系统调用
        if (read_size < 0) {                        // 读取错误
            if (errno == EINTR) {
                continue;  // 被信号中断时自动重试，保证读取完整性
            }
            status = PosixError(filename_, errno);
            break;
        }
        *result = std::string_view(scratch, read_size);  // 返回读取的数据
        break;
    }

    return status;
}

/**
 * 跳过字节
 * 使用 lseek() 移动文件指针
 * SEEK_CUR 表示相对于当前位置偏移
 * 不会触发 I/O：纯指针操作，比读取相同数据更快
 */
Status SequentialFile::Skip(uint64_t n) {
    if (lseek(fd_, n, SEEK_CUR) == static_cast<off_t>(-1)) {
        return PosixError(filename_, errno);
    }
    return Status::OK();
}

}  // namespace delta