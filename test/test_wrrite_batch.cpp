#include <gtest/gtest.h>

#include <deltadb/db/db_impl.h>
#include <deltadb/db/memtable.h>
#include <deltadb/db/write_batch.h>
#include <deltadb/db/write_batch_internal.h>

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

static std::string PrintContents(WriteBatch* batch) {
    // 创建 MemTable 作为输出目标
    InternalKeyComparator cmp(BytewiseComparator());
    MemTable* mem = new MemTable(cmp);
    mem->Ref();  // 增加引用计数

    // 调用 InsertInto 将 batch 插入 MemTable
    std::string state;
    Status status = WriteBatchInternal::InsertInto(batch, mem);

    // 遍历 MemTable，格式化输出每个操作
    int count = 0;
    Iterator* iter = mem->NewIterator();
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        ParsedInternalKey ikey{};
        EXPECT_TRUE(ParseInternalKey(iter->key(), &ikey));
        switch (ikey.type) {
            case kTypeValue:
                state.append("Put(");
                state.append(ikey.user_key);
                state.append(", ");
                state.append(iter->value());
                state.append(")");
                count++;
                break;
            case kTypeDeletion:
                state.append("Delete(");
                state.append(ikey.user_key);
                state.append(")");
                count++;
                break;
        }
        state.append("@");
        state.append(std::format("{}", ikey.sequence));
    }
    delete iter;

    // 添加错误信息
    if (!status.ok()) {
        state.append("ParseError()");
    } else if (count != WriteBatchInternal::Count(batch)) {
        state.append("CountMismatch()");
    }
    mem->Unref();
    return state;
}

TEST(WriteBatchTest, Empty) {
    start();

    WriteBatch batch;
    ASSERT_EQ("", PrintContents(&batch));
    ASSERT_EQ(0, WriteBatchInternal::Count(&batch));
}

TEST(WriteBatchTest, Multiple) {
    start();

    WriteBatch batch;
    batch.Put(std::string_view("foo"), std::string_view("bar"));
    batch.Delete(std::string_view("box"));
    batch.Put(std::string_view("baz"), std::string_view("boo"));
    WriteBatchInternal::SetSequence(&batch, 100);
    ASSERT_EQ(100, WriteBatchInternal::Sequence(&batch));
    ASSERT_EQ(3, WriteBatchInternal::Count(&batch));
    ASSERT_EQ(
        "Put(baz, boo)@102"
        "Delete(box)@101"
        "Put(foo, bar)@100",
        PrintContents(&batch));
}

TEST(WriteBatchTest, Corruption) {
    WriteBatch batch;
    batch.Put(std::string_view("foo"), std::string_view("bar"));
    batch.Delete(std::string_view("box"));
    WriteBatchInternal::SetSequence(&batch, 200);
    std::string_view contents = WriteBatchInternal::Contents(&batch);
    WriteBatchInternal::SetContents(&batch, std::string_view(contents.data(), contents.size() - 1));
    ASSERT_EQ(
        "Put(foo, bar)@200"
        "ParseError()",
        PrintContents(&batch));
}

TEST(WriteBatchTest, Append) {
    WriteBatch b1, b2;
    WriteBatchInternal::SetSequence(&b1, 200);
    WriteBatchInternal::SetSequence(&b2, 300);
    b1.Append(b2);
    ASSERT_EQ("", PrintContents(&b1));
    b2.Put("a", "va");
    b1.Append(b2);
    ASSERT_EQ("Put(a, va)@200", PrintContents(&b1));
    b2.Clear();
    b2.Put("b", "vb");
    b1.Append(b2);
    ASSERT_EQ(
        "Put(a, va)@200"
        "Put(b, vb)@201",
        PrintContents(&b1));
    b2.Delete("foo");
    b1.Append(b2);
    ASSERT_EQ(
        "Put(a, va)@200"
        "Put(b, vb)@202"
        "Put(b, vb)@201"
        "Delete(foo)@203",
        PrintContents(&b1));
}

TEST(WriteBatchTest, ApproximateSize) {
    WriteBatch batch;
    size_t empty_size = batch.ApproximateSize();

    batch.Put(std::string_view("foo"), std::string_view("bar"));
    size_t one_key_size = batch.ApproximateSize();
    ASSERT_LT(empty_size, one_key_size);

    batch.Put(std::string_view("baz"), std::string_view("boo"));
    size_t two_keys_size = batch.ApproximateSize();
    ASSERT_LT(one_key_size, two_keys_size);

    batch.Delete(std::string_view("box"));
    size_t post_delete_size = batch.ApproximateSize();
    ASSERT_LT(two_keys_size, post_delete_size);
}