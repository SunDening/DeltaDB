#include <gtest/gtest.h>

#include "arena.h"
#include "db_impl.h"

using namespace delta;

TEST(ArenaTest, Basic) {
    delta::start();

    Arena arena;

    // 分配小对象
    [[maybe_unused]] char* p1 = arena.Allocate(16);
    [[maybe_unused]] char* p2 = arena.Allocate(512);
    [[maybe_unused]] char* p3 = arena.Allocate(1024);

    // 检查内存使用量
    size_t expected_usage = 4096 + sizeof(char*);  // 1块 + 1个块指针
    EXPECT_EQ(arena.MemoryUsage(), expected_usage);

    // 分配大对象（超过1KB）
    [[maybe_unused]] char* p4 =
        arena.Allocate(2048);  // 大对象. 当前块剩余容量应该还够，不用分配新块，也就不用增加内存使用量和块指针
    EXPECT_EQ(arena.MemoryUsage(), expected_usage);

    // 分配更大的对象，其大小超过当前块剩余容量，且大于 1KB，因此会分配一个独立块，增加一个块指针
    [[maybe_unused]] char* p5 = arena.Allocate(2500);
    expected_usage += 2500 + sizeof(char*);  // 大对象 + 块指针
    EXPECT_EQ(arena.MemoryUsage(), expected_usage);

    // 分配一个大对象，其大小超过当前块剩余容量，但不超过 1KB，因此会分配一个新块，增加一个块指针
    [[maybe_unused]] char* p6 = arena.Allocate(1023);
    expected_usage += 4096 + sizeof(char*);  // 大对象 + 块指针
    EXPECT_EQ(arena.MemoryUsage(), expected_usage);

    // 分配一个超大对象
    [[maybe_unused]] char* p7 = arena.Allocate(9999);  // 超大对象. 直接分配一个独立块，增加一个块指针
    expected_usage += 9999 + sizeof(char*);            // 超大对象 + 块指针
    EXPECT_EQ(arena.MemoryUsage(), expected_usage);
}