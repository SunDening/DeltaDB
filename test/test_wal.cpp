#include <gtest/gtest.h>
#include <unistd.h>
#include <memory>

#include <fcntl.h>

#include "db_impl.h"
#include "status.h"
#include "util.h"
#include "wal_reader.h"
#include "wal_writer.h"

using namespace delta;

TEST(WalTest, status) {
    delta::start();

    // 成功状态
    Status s1;
    assert(s1.ok());
    std::cout << s1.ToString() << std::endl;  // "OK"

    // 错误状态（使用 string_view 隐式构造）
    Status s2 = Status::NotFound("Key", "not found in L0");
    std::cout << s2.ToString() << std::endl;  // "NotFound[key:not found in L0]"

    // 从各种字符串类型构造（自动转为 string_view）
    std::string msg1 = "disk error";
    const char* msg2 = "sector 7";
    Status s3 = Status::IOError(msg1, msg2);

    // 检查特定错误
    if (s3.IsIOError()) {
        std::cout << "处理 IO 错误" << std::endl;
    }

    // 移动语义（零拷贝）
    Status s4 = std::move(s3);
    assert(s3.ok());  // s3 被移空，变为 OK
}

TEST(WalTest, writer) {
    std::cout << "=== wal Writer Test ===" << std::endl;
    delta::start();

    std::string filename = std::format("{}{}", gDBConfig->wal_path, gDBConfig->wal_base_name);

    int fd = delta::open_file_posix(filename.c_str(), O_WRONLY | O_APPEND);
    std::cout << "open wal_test.wal success fd=" << fd << std::endl;

    auto dest = std::make_shared<WritableFile>(filename, fd);
    std::cout << "init dest success" << std::endl;

    auto writer = std::make_shared<Writer>(dest.get(), 0);
    std::cout << "init writer success" << std::endl;

    Status s;
    std::string small_data = "Hello, Hello!";
    s = writer->AddRecord(small_data);
    std::cout << "write small data " << (s.ok() ? "success" : "failed") << std::endl;

    // 创建一个跨越多个块的记录
    std::vector<char> large_data(50000, 'A');  // 50KB，超过 32KB 块大小
    s = writer->AddRecord(std::string_view(large_data.data(), large_data.size()));
    std::cout << "write small data " << (s.ok() ? "success" : "failed") << std::endl;

    std::string small_data2 = "Hello, Kitty!";
    s = writer->AddRecord(small_data2);
    std::cout << "write small data " << (s.ok() ? "success" : "failed") << std::endl;

    // 创建一个跨越多个块的记录
    std::vector<char> large_data2(50000, 'B');  // 50KB，超过 32KB 块大小
    s = writer->AddRecord(std::string_view(large_data2.data(), large_data2.size()));
    std::cout << "write small data " << (s.ok() ? "success" : "failed") << std::endl;
}

TEST(WalTest, reader) {
    std::cout << "=== wal Writer Test ===" << std::endl;
    delta::start();

    std::string filename = std::format("{}{}", gDBConfig->wal_path, gDBConfig->wal_base_name);

    int fd = delta::open_file_posix(filename.c_str(), O_RDONLY);

    auto file = std::make_shared<delta::SequentialFile>(filename, fd);
    auto reader = std::make_shared<delta::Reader>(file.get(), nullptr, true, 0);
    std::string tmp = "tmp";
    std::string_view sv = "hello";
    std::string_view* record = &sv;
    std::string* scratch = &tmp;

    // 继续读取直到文件末尾
    while (reader->ReadRecord(record, scratch)) {
        std::cout << *record << std::endl;
    }

}