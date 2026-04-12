#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace delta {

class Status {
   public:
    // 默认构造：成功状态
    Status() noexcept : state_(nullptr) {}

    ~Status() { delete[] state_; }

    // 拷贝构造
    Status(const Status& rhs);
    Status& operator=(const Status& rhs);

    // 移动构造
    Status(Status&& rhs) noexcept : state_(rhs.state_) { rhs.state_ = nullptr; }
    Status& operator=(Status&& rhs) noexcept;

    // 工厂方法：返回成功状态
    static Status OK() { return Status(); }

    // 错误状态工厂方法（使用 std::string_view 替代 Slice）
    static Status NotFound(std::string_view msg, std::string_view msg2 = {}) { return Status(kNotFound, msg, msg2); }

    static Status Corruption(std::string_view msg, std::string_view msg2 = {}) {
        return Status(kCorruption, msg, msg2);
    }

    static Status NotSupported(std::string_view msg, std::string_view msg2 = {}) {
        return Status(kNotSupported, msg, msg2);
    }

    static Status InvalidArgument(std::string_view msg, std::string_view msg2 = {}) {
        return Status(kInvalidArgument, msg, msg2);
    }

    static Status IOError(std::string_view msg, std::string_view msg2 = {}) { return Status(kIOError, msg, msg2); }

    // 查询状态
    bool ok() const { return state_ == nullptr; }
    bool IsNotFound() const { return code() == kNotFound; }
    bool IsCorruption() const { return code() == kCorruption; }
    bool IsIOError() const { return code() == kIOError; }
    bool IsNotSupportedError() const { return code() == kNotSupported; }
    bool IsInvalidArgument() const { return code() == kInvalidArgument; }

    // 转换为字符串
    std::string ToString() const;

   private:
    enum Code : uint8_t {
        kOk = 0,
        kNotFound = 1,
        kCorruption = 2,
        kNotSupported = 3,
        kInvalidArgument = 4,
        kIOError = 5
    };

    Code code() const { return (state_ == nullptr) ? kOk : static_cast<Code>(state_[0]); }

    // 核心构造：使用 std::string_view 替代 Slice
    Status(Code code, std::string_view msg, std::string_view msg2);

    static char* CopyState(const char* s);

    // 内存布局（简化版）：
    // state_[0]     == code (1 byte)
    // state_[1..4]  == message length (4 bytes, little-endian)
    // state_[5..]   == message (包含两个 string_view 拼接的内容)
    const char* state_;
};

inline Status PosixError(const std::string& context, int error_number) {
    if (error_number == ENOENT) {
        return Status::NotFound(context, std::strerror(error_number));
    } else {
        return Status::IOError(context, std::strerror(error_number));
    }
}

}  // namespace delta