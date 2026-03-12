#pragma once
#include <vector>
#include <string>
#include <xxhash.h>
#include <iostream>

using namespace std;

class BloomFilter {
public:
    /**
     * 初始化：n=预期键数量, p=误判率
     * n : 预期要存储的key的数量（每个sst中），也即布隆过滤器需要处理的元素数量的预估
     * p : 可接受的误判率（假阳性），范围：（0 < p < 1），如0.01
    */
    BloomFilter(size_t n, double p = 0.01);
    
    // 添加键
    void add(const std::string& key);
    
    // 检查键是否存在（可能误判）
    bool contains(const std::string& key) const;
    
    // 返回位数组大小（bytes）
    size_t size() const { return m_bits.size(); }

private:
    size_t m_size;          // 位数组大小（bits）
    size_t m_num_hashes;    // 哈希函数个数
    std::vector<uint8_t> m_bits; // 位数组（按字节存储）

    // 计算哈希值（使用 xxHash）
    uint64_t hash(const std::string& key, uint32_t seed) const;
};