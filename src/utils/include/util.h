#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

#include "coding.h"
#include "status.h"

namespace delta {
// ======================= LogLevel begin =============================
enum LogLevel { DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, NONE = 5 };

inline std::string levelToString(LogLevel level) {
    std::string re = "DEBUG";
    switch (level) {
        case DEBUG:
            re = "DEBUG";
            return re;

        case INFO:
            re = "INFO";
            return re;

        case WARN:
            re = "WARN";
            return re;

        case ERROR:
            re = "ERROR";
            return re;
        case NONE:
            re = "NONE";

        default:
            return re;
    }
}

inline LogLevel stringToLevel(const std::string& str) {
    if (str == "DEBUG") return LogLevel::DEBUG;

    if (str == "INFO") return LogLevel::INFO;

    if (str == "WARN") return LogLevel::WARN;

    if (str == "ERROR") return LogLevel::ERROR;

    if (str == "NONE") return LogLevel::NONE;

    return LogLevel::DEBUG;
}

// ======================= LogLevel end =============================

// ======================= LogType begin =============================
enum LogType {
    DB_LOG = 1,
    APP_LOG = 2,
};

inline std::string LogTypeToString(LogType logtype) {
    switch (logtype) {
        case APP_LOG:
            return "app";
        case DB_LOG:
            return "db";
        default:
            return "";
    }
}
// ======================= LogType begin =============================

// ======================= open file begin ===========================
/**
 * 打开文件，如果文件不存在则创建
 * @param filename 文件名
 * @param flags 打开标志（默认 只写 | 清零）
 * @param mode 创建文件的权限（默认 0644）
 * @return 文件描述符，失败返回 -1
 */
inline int open_file_posix(const char* filename, int flags = O_WRONLY | O_TRUNC,
                           mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) {
    // 先尝试不创建的方式打开
    int fd = open(filename, flags & ~O_CREAT);  // 确保不使用 O_CREAT

    if (fd == -1) {
        if (errno == ENOENT) {
            // 文件不存在，提示用户
            std::cout << "File '" << filename << "' does not exist. ";
            std::cout << "Creating new file with mode " << std::oct << mode << std::dec << "..." << std::endl;

            // 添加 O_CREAT 标志重新打开
            fd = open(filename, flags | O_CREAT | O_TRUNC, mode);

            if (fd == -1) {
                std::cerr << "Failed to create file: " << filename << std::endl;
                std::cerr << "Error: " << strerror(errno) << std::endl;
                return -1;
            }

            std::cout << "File created successfully." << std::endl;
        } else {
            // 其他错误
            std::cerr << "Failed to open file: " << filename << std::endl;
            std::cerr << "Error: " << strerror(errno) << std::endl;
            return -1;
        }
    }

    std::cout << "File descriptor: " << fd << std::endl;

    // 获取并显示文件信息（可选）
    struct stat st;
    if (fstat(fd, &st) == 0) {
        std::cout << "File size: " << st.st_size << " bytes" << std::endl;
    }

    return fd;
}
// ======================= open file end ===========================

class CondVar {
   public:
    explicit CondVar(std::mutex* mu) : mtx_(mu) { assert(mu != nullptr); }
    ~CondVar() = default;

    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

    void Wait() {
        std::unique_lock<std::mutex> lock(*mtx_, std::adopt_lock);
        cv_.wait(lock);
        lock.release();
    }
    void Signal() { cv_.notify_one(); }
    void SignalAll() { cv_.notify_all(); }

   private:
    std::condition_variable cv_;
    std::mutex* const mtx_;
};

// 解析十进制数字
inline bool ConsumeDecimalNumber(std::string_view* in, uint64_t* val) {
    // Constants that will be optimized away.
    constexpr const uint64_t kMaxUint64 = std::numeric_limits<uint64_t>::max();
    constexpr const char kLastDigitOfMaxUint64 = '0' + static_cast<char>(kMaxUint64 % 10);

    uint64_t value = 0;

    // reinterpret_cast-ing from char* to uint8_t* to avoid signedness.
    const uint8_t* start = reinterpret_cast<const uint8_t*>(in->data());

    const uint8_t* end = start + in->size();
    const uint8_t* current = start;
    for (; current != end; ++current) {
        const uint8_t ch = *current;
        if (ch < '0' || ch > '9') break;

        // Overflow check.
        // kMaxUint64 / 10 is also constant and will be optimized away.
        if (value > kMaxUint64 / 10 || (value == kMaxUint64 / 10 && ch > kLastDigitOfMaxUint64)) {
            return false;
        }

        value = (value * 10) + (ch - '0');
    }

    *val = value;
    const size_t digits_consumed = current - start;
    in->remove_prefix(digits_consumed);
    return digits_consumed != 0;
}

inline Status CreateDir(const std::string& dirname) {
    if (::mkdir(dirname.c_str(), 0755) != 0) {
        return PosixError(dirname, errno);
    }
    return Status::OK();
}

inline bool FileExists(const std::string& filename) { return ::access(filename.c_str(), F_OK) == 0; }

inline Status GetChildren(const std::string& directory_path, std::vector<std::string>* result) {
    result->clear();
    ::DIR* dir = ::opendir(directory_path.c_str());
    if (dir == nullptr) {
        return PosixError(directory_path, errno);
    }
    struct ::dirent* entry;
    while ((entry = ::readdir(dir)) != nullptr) {
        result->emplace_back(entry->d_name);
    }
    ::closedir(dir);
    return Status::OK();
}

// 文件重命名
inline Status RenameFile(const std::string& from, const std::string& to) {
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        return PosixError(from, errno);
    }
    return Status::OK();
}

inline Status RemoveFile(const std::string& fname) {
    if (std::remove(fname.c_str()) != 0) {
        return PosixError(fname, errno);
    }
    return Status::OK();
}

inline Status RemoveDir(const std::string& dirname) {
    if (::rmdir(dirname.c_str()) != 0) {
        return PosixError(dirname, errno);
    }
    return Status::OK();
}

/**
 * @brief 将数据写入命名文件并同步到磁盘
 *
 * @param data：要写入的数据
 * @param fname：目标文件名
 */
inline Status WriteStringToFileSync(const std::string_view& data, const std::string& fname) {
    // 创建/截断文件，权限 0644
    int fd = ::open(fname.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return PosixError(fname, errno);
    }

    // 写入数据
    const char* ptr = data.data();
    size_t left = data.size();
    while (left > 0) {
        ssize_t written = ::write(fd, ptr, left);
        if (written < 0) {
            ::close(fd);
            return PosixError(fname, errno);
        }
        ptr += written;
        left -= written;
    }

    // 刷盘
    if (::fsync(fd) < 0) {
        ::close(fd);
        return PosixError(fname, errno);
    }

    ::close(fd);
    return Status::OK();
}

inline uint64_t NowMicros() {
    static constexpr uint64_t kUsecondsPerSecond = 1000000;
    struct ::timeval tv;
    ::gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * kUsecondsPerSecond + tv.tv_usec;
}

inline uint32_t Hash(const char* data, size_t n, uint32_t seed) {
    // Similar to murmur hash
    const uint32_t m = 0xc6a4a793;
    const uint32_t r = 24;
    const char* limit = data + n;
    uint32_t h = seed ^ (n * m);

    // Pick up four bytes at a time
    while (limit - data >= 4) {
        uint32_t w = DecodeFixed32(data);
        data += 4;
        h += w;
        h *= m;
        h ^= (h >> 16);
    }

    // Pick up remaining bytes
    switch (limit - data) {
        case 3:
            h += static_cast<uint8_t>(data[2]) << 16;
            [[fallthrough]];
        case 2:
            h += static_cast<uint8_t>(data[1]) << 8;
            [[fallthrough]];
        case 1:
            h += static_cast<uint8_t>(data[0]);
            h *= m;
            h ^= (h >> r);
            break;
    }
    return h;
}

inline bool Snappy_Compress(const char* input, size_t length, std::string* output) {
#if HAVE_SNAPPY
    output->resize(snappy::MaxCompressedLength(length));
    size_t outlen;
    snappy::RawCompress(input, length, &(*output)[0], &outlen);
    output->resize(outlen);
    return true;
#else
    // Silence compiler warnings about unused arguments.
    (void)input;
    (void)length;
    (void)output;
#endif  // HAVE_SNAPPY

    return false;
}

inline bool Snappy_GetUncompressedLength(const char* input, size_t length, size_t* result) {
#if HAVE_SNAPPY
    return snappy::GetUncompressedLength(input, length, result);
#else
    // Silence compiler warnings about unused arguments.
    (void)input;
    (void)length;
    (void)result;
    return false;
#endif  // HAVE_SNAPPY
}

inline bool Snappy_Uncompress(const char* input, size_t length, char* output) {
#if HAVE_SNAPPY
    return snappy::RawUncompress(input, length, output);
#else
    // Silence compiler warnings about unused arguments.
    (void)input;
    (void)length;
    (void)output;
    return false;
#endif  // HAVE_SNAPPY
}

inline bool Zstd_Compress(int level, const char* input, size_t length, std::string* output) {
#if HAVE_ZSTD
    // Get the MaxCompressedLength.
    size_t outlen = ZSTD_compressBound(length);
    if (ZSTD_isError(outlen)) {
        return false;
    }
    output->resize(outlen);
    ZSTD_CCtx* ctx = ZSTD_createCCtx();
    ZSTD_compressionParameters parameters = ZSTD_getCParams(level, std::max(length, size_t{1}), /*dictSize=*/0);
    ZSTD_CCtx_setCParams(ctx, parameters);
    outlen = ZSTD_compress2(ctx, &(*output)[0], output->size(), input, length);
    ZSTD_freeCCtx(ctx);
    if (ZSTD_isError(outlen)) {
        return false;
    }
    output->resize(outlen);
    return true;
#else
    // Silence compiler warnings about unused arguments.
    (void)level;
    (void)input;
    (void)length;
    (void)output;
    return false;
#endif  // HAVE_ZSTD
}

inline bool Zstd_GetUncompressedLength(const char* input, size_t length, size_t* result) {
#if HAVE_ZSTD
    size_t size = ZSTD_getFrameContentSize(input, length);
    if (size == 0) return false;
    *result = size;
    return true;
#else
    // Silence compiler warnings about unused arguments.
    (void)input;
    (void)length;
    (void)result;
    return false;
#endif  // HAVE_ZSTD
}

inline bool Zstd_Uncompress(const char* input, size_t length, char* output) {
#if HAVE_ZSTD
    size_t outlen;
    if (!Zstd_GetUncompressedLength(input, length, &outlen)) {
        return false;
    }
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    outlen = ZSTD_decompressDCtx(ctx, output, outlen, input, length);
    ZSTD_freeDCtx(ctx);
    if (ZSTD_isError(outlen)) {
        return false;
    }
    return true;
#else
    // Silence compiler warnings about unused arguments.
    (void)input;
    (void)length;
    (void)output;
    return false;
#endif  // HAVE_ZSTD
}

inline void SleepForMicroseconds(int micros) { std::this_thread::sleep_for(std::chrono::microseconds(micros)); }

inline Status GetFileSize(const std::string& filename, uint64_t* size) {
    struct ::stat file_stat;
    if (::stat(filename.c_str(), &file_stat) != 0) {
        *size = 0;
        return PosixError(filename, errno);
    }
    *size = file_stat.st_size;
    return Status::OK();
}

// ====================== random ======================
class Random {
   private:
    uint32_t seed_;

   public:
    explicit Random(uint32_t s) : seed_(s & 0x7fffffffu) {
        // Avoid bad seeds.
        if (seed_ == 0 || seed_ == 2147483647L) {
            seed_ = 1;
        }
    }
    uint32_t Next() {
        static const uint32_t M = 2147483647L;  // 2^31-1
        static const uint64_t A = 16807;        // bits 14, 8, 7, 5, 2, 1, 0
        // We are computing
        //       seed_ = (seed_ * A) % M,    where M = 2^31-1
        //
        // seed_ must not be zero or M, or else all subsequent computed values
        // will be zero or M respectively.  For all other values, seed_ will end
        // up cycling through every number in [1,M-1]
        uint64_t product = seed_ * A;

        // Compute (product % M) using the fact that ((x << 31) % M) == x.
        seed_ = static_cast<uint32_t>((product >> 31) + (product & M));
        // The first reduction may overflow by 1 bit, so we may need to
        // repeat.  mod == M is not possible; using > allows the faster
        // sign-bit-based test.
        if (seed_ > M) {
            seed_ -= M;
        }
        return seed_;
    }
    // Returns a uniformly distributed value in the range [0..n-1]
    // REQUIRES: n > 0
    uint32_t Uniform(int n) { return Next() % n; }

    // Randomly returns true ~"1/n" of the time, and false otherwise.
    // REQUIRES: n > 0
    bool OneIn(int n) { return (Next() % n) == 0; }

    // Skewed: pick "base" uniformly from range [0,max_log] and then
    // return "base" random bits.  The effect is to pick a number in the
    // range [0,2^max_log-1] with exponential bias towards smaller numbers.
    uint32_t Skewed(int max_log) { return Uniform(1 << Uniform(max_log + 1)); }
};

}  // namespace delta
