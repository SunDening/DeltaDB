#include <unistd.h>

#include <fcntl.h>
#include "writable_file.h"

namespace delta {

std::string_view WritableFile::Basename(const std::string& filename) {
    std::string::size_type separator_pos = filename.rfind('/');
    if (separator_pos == std::string::npos) {
        return std::string_view(filename);
    }
    // The filename component should not contain a path separator. If it does,
    // the splitting was done incorrectly.
    assert(filename.find('/', separator_pos + 1) == std::string::npos);

    return std::string_view(filename.data() + separator_pos + 1, filename.length() - separator_pos - 1);
}

std::string WritableFile::Dirname(const std::string& filename) {
    std::string::size_type separator_pos = filename.rfind('/');
    if (separator_pos == std::string::npos) {
        return std::string(".");
    }
    // The filename component should not contain a path separator. If it does,
    // the splitting was done incorrectly.
    assert(filename.find('/', separator_pos + 1) == std::string::npos);

    return filename.substr(0, separator_pos);
}

bool WritableFile::IsManifest(const std::string& filename) { return Basename(filename).starts_with("MANIFEST"); }

/**
 * 刷新缓冲区状态：
 *  1. 将当前缓冲区已有内容写入文件
 *  2. 将缓冲区指针重置归零
 */
Status WritableFile::FlushBuffer() {
    Status status = WriteUnbuffered(buf_, pos_);
    pos_ = 0;  // 重置缓冲区指针
    return status;
}

/**
 * 将 data 写入操作系统页缓存，不能保证持久到磁盘
 * data 可能来自缓冲区，也可能是比较大不走缓冲区的原始 data
 */
Status WritableFile::WriteUnbuffered(const char* data, size_t size) {
    while (size > 0) {
        ssize_t write_result = ::write(fd_, data, size);
        if (write_result < 0) {
            if (errno == EINTR) {
                continue;  // Retry 中断重试
            }
            return PosixError(filename_, errno);
        }
        data += write_result;
        size -= write_result;
    }
    return Status::OK();
}

Status WritableFile::SyncDirIfManifest() {
    if (!is_manifest_) {
        return Status::OK();
    }

    /**
     * Manifest 文件需要同步父目录:
     * Manifest 记录了数据库的文件结构
     * 创建新文件后，目录项在内存中
     * 不同步目录可能导致崩溃后找不到文件
     */
    int fd = ::open(dirname_.c_str(), O_RDONLY | kOpenBaseFlags);
    if (fd < 0) {
        return PosixError(dirname_, errno);
    }
    Status status = SyncFd(fd, dirname_);
    ::close(fd);
    return status;
}

/**
 * fsync(fd) 在 Linux 中的作用是强制将文件描述符 fd
 * 所指向文件的所有修改过的数据和元数据（metadata）从内核缓冲区立即写入到物理存储设备（如磁盘）上，
 * 并等待写入完成才返回
 */
Status WritableFile::SyncFd(int fd, const std::string& fd_path) {
    // Linux 使用 fsync
    if (::fsync(fd) != 0) {
        return PosixError(fd_path, errno);
    }
    return Status::OK();
}

/**
 * 追加数据到缓冲区
 * 设计策略：
 *  小写入（< 64KB）：缓冲到内存，批量写入
 *  大写入（>= 64KB）：绕过缓冲区，直接写入
 */
Status WritableFile::Append(const std::string_view data) {
    size_t write_size = data.size();
    const char* write_data = data.data();

    // 尝试将数据填充到缓冲区
    size_t copy_size = std::min(write_size, kWritableFileBufferSize - pos_);  // 一次写入的大小
    std::memcpy(buf_ + pos_, write_data, copy_size);
    write_data += copy_size;  // 右移指针到第一个未写入的数据
    write_size -= copy_size;  // 仍需要写入的数据
    pos_ += copy_size;        // 右移缓冲区指针

    if (write_size == 0) {
        return Status::OK();  // 数据完全写入缓冲区
    }

    // 仍有需要写入的数据但缓冲区已满（如果没满肯定已经写完了），先刷新缓冲区
    Status status = FlushBuffer();
    if (!status.ok()) {
        return status;
    }

    // 剩余数据：小写入进缓冲区，大写入直接写入. 防止data过大时多次刷新缓冲区
    if (write_size < kWritableFileBufferSize) {
        std::memcpy(buf_, write_data, write_size);
        pos_ = write_size;
        return Status::OK();
    }
    return WriteUnbuffered(write_data, write_size);
}

/**
 * 数据进入页缓存
 */
Status WritableFile::Flush() { return FlushBuffer(); }

/**
 * 确保数据写入磁盘
 */
Status WritableFile::Sync() {
    // 确保 manifest 文件引用的新文件在文件系统中
    Status status = SyncDirIfManifest();
    if (!status.ok()) {
        return status;
    }

    // 刷新缓冲区
    status = FlushBuffer();
    if (!status.ok()) {
        return status;
    }

    // 同步文件描述符到磁盘
    return SyncFd(fd_, filename_);
}

Status WritableFile::Close() {
    Status status = FlushBuffer();  // 先刷新缓冲区
    const int close_result = ::close(fd_);
    if (close_result < 0 && status.ok()) {
        status = PosixError(filename_, errno);
    }
    fd_ = -1;
    return status;
}

}  // namespace delta