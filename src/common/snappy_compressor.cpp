#include "snappy_compressor.h"
#include <snappy.h>

bool SnappyCompressor::compress(const std::string& input, std::string& output) {
    try {
        snappy::Compress(input.data(), input.size(), &output);
        return true;
    } catch (...) {
        return false;
    }
}

bool SnappyCompressor::decompress(const std::string& input, std::string& output) {
    try {
        if (!snappy::IsValidCompressedBuffer(input.data(), input.size())) {
            return false;
        }
        return snappy::Uncompress(input.data(), input.size(), &output);
    } catch (...) {
        return false;
    }
}
