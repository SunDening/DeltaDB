#include <algorithm>
#include <cmath>

#include "bloom.h"

namespace delta {
BloomFilter::BloomFilter(size_t n, double p) {
    if (n == 0 || p <= 0.0 || p >= 1.0) {
        throw std::invalid_argument("Invalid BloomFilter parameters");
    }
    double ln2 = std::log(2.0);
    double m = -static_cast<double>(n) * std::log(p) / (ln2 * ln2);
    double k = (m / n) * ln2;

    m_size = static_cast<size_t>(std::ceil(m));
    m_num_hashes = static_cast<size_t>(std::ceil(k));

    m_bits.resize((m_size + 7) / 8, 0);
}

void BloomFilter::add(const std::string &key) {
    for (size_t i = 0; i < m_num_hashes; ++i) {
        uint64_t h =
            hash(key, i) % m_size;  // 计算哈希位置。通过给xxHash传入不同的seed模拟多个哈希函数，避免写多个独立hash函数
        m_bits[h / 8] |= (1 << (h % 8));  // 设置位
    }
}

bool BloomFilter::contains(const std::string &key) const {
    for (size_t i = 0; i < m_num_hashes; ++i) {
        uint64_t h = hash(key, i) % m_size;
        if (!(m_bits[h / 8] & (1 << (h % 8)))) {
            return false;  // 如果某一位未设置，键一定不存在
        }
    }
    return true;  // 所有位已设置，键可能存在（可能有误判）
}

uint64_t BloomFilter::hash(const std::string &key, uint32_t seed) const {
    return XXH64(key.data(), key.size(), seed);  // 使用 xxHash
}
}  // namespace delta
