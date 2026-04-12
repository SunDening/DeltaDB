#include <gtest/gtest.h>
#include <random>

#include <deltadb/db/db_impl.h>

using namespace delta;

static void start() {
    // Try multiple config paths: project root first, then parent directory
    const char* config_paths[] = {"./conf/config.xml", "../conf/config.xml", "../../conf/config.xml"};
    const char* chosen_path = config_paths[0];
    for (const char* path : config_paths) {
        if (access(path, F_OK) == 0) {
            chosen_path = path;
            break;
        }
    }
    gDBConfig = std::make_shared<Config>(chosen_path);
    gDBLogger = std::make_shared<Logger>();
    gDBLogger->start();
}

TEST(DBTest, Put2) {
    // 1. 初始化全局配置和日志模块
    start();

    // 2. 打开数据库
    delta::DB* db = nullptr;
    Status s = delta::DB::Open("./output/delta_compaction_test", &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
        return;
    }

    // 3. 随机生成20万对K-V并Put，预计触发多次MemTable flush和L0 compaction
    std::mt19937 gen(42);  // 固定种子保证可重复性
    std::uniform_int_distribution<> key_dist(1, 1000000);
    std::uniform_int_distribution<> value_dist(1, 100000);

    const int total_puts = 200000;
    int success_count = 0;

    for (int i = 0; i < total_puts; ++i) {
        std::string key = "key_" + std::to_string(key_dist(gen));
        std::string value = "value_" + std::to_string(value_dist(gen));

        Status status = db->Put(WriteOptions(), key, value);
        if (status.ok()) {
            ++success_count;
        }

        // 每50000次打印进度
        if ((i + 1) % 50000 == 0) {
            std::cout << "Progress: " << (i + 1) << " / " << total_puts << " (success: " << success_count << ")"
                      << std::endl;
        }
    }

    std::cout << "Successfully put " << success_count << " / " << total_puts << " key-value pairs" << std::endl;
    EXPECT_EQ(success_count, total_puts);

    // 4. 关闭数据库
    delete db;
}

TEST(DBTest, Basic) {
    // 1. 初始化全局配置和日志模块
    start();

    // 2. 打开数据库
    delta::DB* db = nullptr;
    Status s = delta::DB::Open("./output/delta", &db);
    if (!s.ok()) {
        std::cerr << "Failed to open DB: " << s.ToString() << std::endl;
        return;
    }

    // 3. 使用数据库...
    // db->Put(...), db->Get(...), etc.
    std::string key = "aaa";
    std::string value = "this is a";
    Status status;

    status = db->Put(WriteOptions(), key, value);

    status = db->Get(ReadOptions(), key, &value);
    std::cout << value << std::endl;

    status = db->Delete(WriteOptions(), key);
    std::cout << "del: " << status.ok() << std::endl;

    status = db->Get(ReadOptions(), key, &value);
    std::cout << "get: " << status.ok() << std::endl;

    // 4. 关闭数据库
    delete db;
}