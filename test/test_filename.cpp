#include <gtest/gtest.h>

#include "filename.h"

using namespace delta;

TEST(FileNameTest, Parse) {
    FileType type;
    uint64_t number;

    // should success
    static struct {
        const char* fname;
        uint64_t number;
        FileType type;
    } cases[] = {
        {"100.wal", 100, kWalFile},       {"0.wal", 0, kWalFile},   {"0.sst", 0, kSSTFile},
        {"CURRENT", 0, kCurrentFile},     {"LOCK", 0, kDBLockFile}, {"MANIFEST-2", 2, kManifestFile},
        {"MANIFEST-7", 7, kManifestFile}, {"LOG", 0, kInfoLogFile}, {"LOG.old", 0, kInfoLogFile},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        std::string f = cases[i].fname;
        // 在测试断言宏（如 ASSERT_TRUE、EXPECT_TRUE 等）中，<< 运算符的作用是输出附加的诊断信息，当断言失败时，<<
        // 后面的内容（f）会被打印出来，帮助定位问题。
        ASSERT_TRUE(ParseFileName(f, &number, &type)) << f;
        ASSERT_EQ(cases[i].type, type) << f;
        ASSERT_EQ(cases[i].number, number) << f;
    }

    // should error
    static const char* errors[] = {"",
                                   "foo",
                                   "foo-dx-100.log",
                                   ".log",
                                   "",
                                   "manifest",
                                   "CURREN",
                                   "CURRENTX",
                                   "MANIFES",
                                   "MANIFEST",
                                   "MANIFEST-",
                                   "XMANIFEST-3",
                                   "MANIFEST-3x",
                                   "LOC",
                                   "LOCKx",
                                   "LO",
                                   "LOGx",
                                   "18446744073709551616.log",
                                   "184467440737095516150.log",
                                   "100",
                                   "100.",
                                   "100.lop"};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        std::string f = errors[i];
        ASSERT_TRUE(!ParseFileName(f, &number, &type)) << f;
    }
}

TEST(FileNameTest, Construction) {
    uint64_t number;
    FileType type;
    std::string fname;

    fname = CurrentFileName("foo");
    ASSERT_EQ("foo/", std::string(fname.data(), 4));  // 前4个字符
    ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
    ASSERT_EQ(0, number);
    ASSERT_EQ(kCurrentFile, type);

    fname = LockFileName("foo");
    ASSERT_EQ("foo/", std::string(fname.data(), 4));
    ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
    ASSERT_EQ(0, number);
    ASSERT_EQ(kDBLockFile, type);

    fname = WalFileName("foo", 192);
    ASSERT_EQ("foo/", std::string(fname.data(), 4));
    ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
    ASSERT_EQ(192, number);
    ASSERT_EQ(kWalFile, type);

    fname = SSTFileName("bar", 200);
    ASSERT_EQ("bar/", std::string(fname.data(), 4));
    ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
    ASSERT_EQ(200, number);
    ASSERT_EQ(kSSTFile, type);

    fname = ManifestFileName("bar", 100);
    ASSERT_EQ("bar/", std::string(fname.data(), 4));
    ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
    ASSERT_EQ(100, number);
    ASSERT_EQ(kManifestFile, type);

    fname = TempFileName("tmp", 999);
    ASSERT_EQ("tmp/", std::string(fname.data(), 4));
    ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
    ASSERT_EQ(999, number);
    ASSERT_EQ(kTempFile, type);

    fname = InfoLogFileName("foo");
    ASSERT_EQ("foo/", std::string(fname.data(), 4));
    ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
    ASSERT_EQ(0, number);
    ASSERT_EQ(kInfoLogFile, type);

    fname = OldInfoLogFileName("foo");
    ASSERT_EQ("foo/", std::string(fname.data(), 4));
    ASSERT_TRUE(ParseFileName(fname.c_str() + 4, &number, &type));
    ASSERT_EQ(0, number);
    ASSERT_EQ(kInfoLogFile, type);
}