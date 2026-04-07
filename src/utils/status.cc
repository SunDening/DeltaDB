#include "status.h"

namespace delta {

Status::Status(const Status& rhs) { state_ = (rhs.state_ == nullptr) ? nullptr : CopyState(rhs.state_); }

Status& Status::operator=(const Status& rhs) {
    if (state_ != rhs.state_) {
        delete[] state_;
        state_ = (rhs.state_ == nullptr) ? nullptr : CopyState(rhs.state_);
    }
    return *this;
}

Status& Status::operator=(Status&& rhs) noexcept {
    std::swap(state_, rhs.state_);
    return *this;
}

// 使用 std::string_view 构造错误状态
Status::Status(Code code, std::string_view msg, std::string_view msg2) {
    // 计算总长度
    const uint32_t len1 = static_cast<uint32_t>(msg.size());
    const uint32_t len2 = static_cast<uint32_t>(msg2.size());
    const uint32_t total_len = len1 + (len2 > 0 ? 1 + len2 : 0);  // +1 for separator

    // 分配内存：1 (code) + 4 (length) + message
    const uint32_t size = 1 + 4 + total_len;
    char* result = new char[size];

    // 编码
    result[0] = static_cast<char>(code);
    // 长度使用小端序存储（与 LevelDB 兼容）
    result[1] = static_cast<char>(total_len & 0xff);
    result[2] = static_cast<char>((total_len >> 8) & 0xff);
    result[3] = static_cast<char>((total_len >> 16) & 0xff);
    result[4] = static_cast<char>((total_len >> 24) & 0xff);

    // 复制消息内容（string_view 可能不连续，必须拷贝）
    std::memcpy(result + 5, msg.data(), len1);
    if (len2 > 0) {
        result[5 + len1] = ':';
        std::memcpy(result + 5 + len1 + 1, msg2.data(), len2);
    }

    state_ = result;
}

char* Status::CopyState(const char* s) {
    // 读取长度（小端序）
    uint32_t length;
    std::memcpy(&length, s + 1, sizeof(length));

    // 总大小 = 1 (code) + 4 (length field) + length (message)
    uint32_t size = 1 + 4 + length;
    char* result = new char[size];
    std::memcpy(result, s, size);
    return result;
}

std::string Status::ToString() const {
    if (state_ == nullptr) {
        return "OK";
    }

    static const char* code_names[] = {"OK", "NotFound", "Corruption", "NotSupported", "InvalidArgument", "IOError"};

    Code c = code();
    const char* code_name = (c <= kIOError) ? code_names[c] : "Unknown";

    // 解码长度
    uint32_t length;
    std::memcpy(&length, state_ + 1, sizeof(length));

    // 构造结果：CodeName[message]
    std::string result(code_name);
    result.push_back('[');
    result.append(state_ + 5, length);  // 从 offset 5 开始，length 字节
    result.push_back(']');

    return result;
}

}