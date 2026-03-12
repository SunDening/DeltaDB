#pragma once

#include "json.hpp"
#include <fstream>
#include <string>
#include <iostream>

using json = nlohmann::json;

struct KVConfig {
    std::string wal_dir = "./wals/";
    std::string log_dir = "./logs/";
    std::string sst_dir = "./ssts/";

    size_t memtable_threshold = 32;   // 单位：根据你自己内部定义
    size_t compact_threshold = 3;     // 触发合并的阈值
    size_t max_file_size = 32;        // 单位：KB？MB？自己统一
    size_t per_kv_size = 8;           // 每条 KV 估算值
};

class ConfigLoader {
public:
    ConfigLoader(const std::string& path) {
        std::ifstream in(path);
        if (!in) {
            std::cerr << "[WARN] Failed to open config file: " << path
                      << ", using default config." << std::endl;
            return;
        }

        try {
            in >> config;
        } catch (const std::exception& e) {
            std::cerr << "[WARN] JSON parse error: " << e.what()
                      << ", using default config." << std::endl;
        }
    }

    KVConfig getKVConfig() {
        KVConfig cfg;

        if (config.contains("kv_config")) {
            auto kv = config["kv_config"];

            if (kv.contains("wal_dir") && kv["wal_dir"].is_string()) {
                cfg.wal_dir = kv["wal_dir"];
            }
            if (kv.contains("log_dir") && kv["log_dir"].is_string()) {
                cfg.log_dir = kv["log_dir"];
            }
            if (kv.contains("sst_dir") && kv["sst_dir"].is_string()) {
                cfg.sst_dir = kv["sst_dir"];
            }
            if (kv.contains("memtable_threshold") && kv["memtable_threshold"].is_number()) {
                cfg.memtable_threshold = kv["memtable_threshold"];
            }
            if (kv.contains("compact_threshold") && kv["compact_threshold"].is_number()) {
                cfg.compact_threshold = kv["compact_threshold"];
            }
            if (kv.contains("max_file_size") && kv["max_file_size"].is_number()) {
                cfg.max_file_size = kv["max_file_size"];
            }
            if (kv.contains("per_kv_size") && kv["per_kv_size"].is_number()) {
                cfg.per_kv_size = kv["per_kv_size"];
            }
        }

        return cfg;
    }

private:
    json config;
};
