#include "filter_policy.h"
#include "util.h"

namespace delta {

// Bloom Filter 专用的哈希函数
static u_int32_t BloomHash(const std::string_view& key) { return Hash(key.data(), key.size(), 0xbc9f1d34); }

class BloomFilterPolicy : public FilterPolicy {
   private:
    size_t bits_per_key_;  // 每个 key 分配的位数
    size_t k_;             // 哈希函数个数

   public:
    explicit BloomFilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
        // 向下取整以减少探测次数
        k_ = static_cast<size_t>(bits_per_key * 0.69);  // 0.69 =~ ln(2)
        if (k_ < 1) k_ = 1;
        if (k_ > 30) k_ = 30;
    }

    const char* Name() const override { return "delta.BuiltinBloomFilter2"; }

    void CreateFilter(const std::string_view* keys, int n, std::string* dst) const override {
        // 计算 Bloom Filter 大小（位数和字节数）
        size_t bits = n * bits_per_key_;

        // 最小长度限制：n 较小时误判率很高，强制至少 64 位
        if (bits < 64) bits = 64;

        // 位数转换为字节数（向上取整），再转回位数（向下对齐到 8 的倍数）
        size_t bytes = (bits + 7) / 8;
        bits = bytes * 8;

        // 预分配空间
        const size_t init_size = dst->size();
        dst->resize(init_size + bytes, 0);      // 字节数组初始化为 0
        dst->push_back(static_cast<char>(k_));  // 末尾追加 k 值

        char* array = &(*dst)[init_size];

        // 为每个 key 设置比特位
        for (int i = 0; i < n; i++) {
            // ================================================================
            // 双哈希（Double Hashing）技术
            // ================================================================
            // 目的：用 2 个哈希值生成 k 个哈希值，减少计算开销
            // 公式：h(i) = h1 + i * h2
            uint32_t h = BloomHash(keys[i]);               // h1
            const uint32_t delta = (h >> 17) | (h << 15);  // h2

            // 设置 k 个比特位
            for (size_t j = 0; j < k_; j++) {
                const uint32_t bitpos = h % bits;          // 计算比特位置
                array[bitpos / 8] |= (1 << (bitpos % 8));  // 设置对应位为 1
                h += delta;
            }
        }
    }

    bool KeyMayMatch(const std::string_view& key, const std::string_view& bloom_filter) const override {
        const size_t len = bloom_filter.size();

        // 最小长度检查：至少需要 2 字节（1 字节数据 + 1 字节 k 值）
        if (len < 2) return false;

        const char* array = bloom_filter.data();

        // 计算位数组的位数（总长度 - 1 字节 k 值）× 8
        const size_t bits = (len - 1) * 8;

        // 读取末尾存储的 k 值。目的：兼容不同参数生成的 Filter
        const size_t k = array[len - 1];

        if (k > 30) {
            // 保留值，可能是新编码格式，保守返回 true
            return true;
        }

        // 计算 key 的哈希值（与 CreateFilter 相同的算法）
        uint32_t h = BloomHash(key);
        const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits

        // 检查 k 个比特位
        for (size_t j = 0; j < k; j++) {
            const uint32_t bitpos = h % bits;

            // 检查对应位是否为 1
            if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
            h += delta;  // 生成下一个哈希值
        }
        // 所有位都是 1 → 可能存在
        return true;
    }
};

// NewBloomFilterPolicy: 工厂函数，创建 Bloom Filter 策略对象
const FilterPolicy* NewBloomFilterPolicy(int bits_per_key) { return new BloomFilterPolicy(bits_per_key); }

}  // namespace delta