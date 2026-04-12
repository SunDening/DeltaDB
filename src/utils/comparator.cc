#include <assert.h>
#include <cstdint>

#include <deltadb/utils/comparator.h>
#include <deltadb/utils/no_destructor.h>

namespace delta {

Comparator::~Comparator() = default;

class BytewiseComparatorImpl : public Comparator {
   public:
    BytewiseComparatorImpl() = default;

    const char* Name() const override { return "delta.BytewiseComparator"; }

    int Compare(const std::string_view& a, const std::string_view& b) const override {
        // 字节序比较
        return a.compare(b);
    }

    void FindShortestSeparator(std::string* start, const std::string_view& limit) const override {
        // 寻找公共前缀长度
        size_t min_length = std::min(start->size(), limit.size());
        size_t diff_index = 0;

        while ((diff_index < min_length) && ((*start)[diff_index] == limit[diff_index])) {
            diff_index++;
        }

        if (diff_index >= min_length) {
            // 一个字符串是另一个的前缀，不缩短
        } else {
            uint8_t diff_byte = static_cast<uint8_t>((*start)[diff_index]);

            // 如果可以增加一个字节并仍然小于 limit
            if (diff_byte < static_cast<uint8_t>(0xff) && diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
                (*start)[diff_index]++;         // 增加字节
                start->resize(diff_index + 1);  // 截断后续部分
                assert(Compare(*start, limit) < 0);
            }
        }
    }

    // 寻找最短后继字符串
    void FindShortSuccessor(std::string* key) const override {
        // 找到第一个可以增加的字节
        size_t n = key->size();

        for (size_t i = 0; i < n; i++) {
            const uint8_t byte = (*key)[i];
            if (byte != static_cast<uint8_t>(0xff)) {
                (*key)[i] = byte + 1;
                key->resize(i + 1);  // 截断后续部分
                return;
            }
        }
        // key 全是 0xff，不处理
    }
};

const Comparator* BytewiseComparator() {
    static NoDestructor<BytewiseComparatorImpl> singleton;
    return singleton.get();
}

}  // namespace delta