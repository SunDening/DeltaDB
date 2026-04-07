#include <gtest/gtest.h>
#include <unistd.h>
#include <memory>

#include "config.h"

static const char* findConfigPath() {
    const char* paths[] = {"./conf/config.xml", "../conf/config.xml", "../../conf/config.xml"};
    for (const char* p : paths) {
        if (access(p, F_OK) == 0) return p;
    }
    return paths[0];  // fallback, will fail gracefully
}

TEST(ConfigTest, Read) {
    auto config = std::make_shared<delta::Config>(findConfigPath());
    config->readConf();

    std::cout << std::format("wal_path:{}, wal_max_file_size:{}", config->wal_path, config->wal_max_file_size)
              << std::endl;
}

TEST(ConfigTest, ReadDBConfig) {
    auto config = std::make_shared<delta::Config>(findConfigPath());
    config->readConf();

    std::cout << std::format(
                     "\n num_levels:{}\n, l0_compaction_trigger:{}\n, l0_slowdown_writes_trigger:{}\n, "
                     "l0_stop_writes_trigger:{}\n, max_mem_compact_level:{}\n, read_bytes_period:{}\n, "
                     "create_if_missing:{}\n, error_if_exists:{}\n, paranoid_checks:{}\n, write_buffer_size:{}\n, "
                     "max_open_files:{}\n, block_size:{}\n, block_restart_internal:{}\n, max_file_size:{}\n, "
                     "zstd_compression_level:{}\n, reuse_logs:{}",
                     config->num_levels, config->l0_compaction_trigger, config->l0_slowdown_writes_trigger,
                     config->l0_stop_writes_trigger, config->max_mem_compact_level, config->read_bytes_period,
                     config->create_if_missing, config->error_if_exists, config->paranoid_checks,
                     config->write_buffer_size, config->max_open_files, config->block_size,
                     config->block_restart_internal, config->max_file_size, config->zstd_compression_level,
                     config->reuse_logs)
              << std::endl;
}