#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <unistd.h>

#include <deltadb/db/db_impl.h>
#include <deltadb/db/memtable.h>
#include <deltadb/db/version_edit.h>
#include <deltadb/table/cache.h>
#include <deltadb/table/sst_builder.h>
#include <deltadb/table/sst_cache.h>

using namespace delta;

namespace {

void InitGlobals() {
    const char* config_paths[] = {"./conf/config.xml", "../conf/config.xml", "../../conf/config.xml"};
    const char* chosen_path = config_paths[0];
    for (const char* path : config_paths) {
        if (access(path, F_OK) == 0) {
            chosen_path = path;
            break;
        }
    }

    gDBConfig = std::make_shared<Config>(chosen_path);
    gDBConfig->compression = kNoCompression;
    if (gDBConfig->block_cache != nullptr) {
        delete gDBConfig->block_cache;
    }
    gDBConfig->block_cache = NewLRUCache(1 << 20);

    gDBLogger = std::make_shared<Logger>();
    gDBLogger->start();
}

std::filesystem::path UniqueTestDir(const char* prefix) {
    static std::atomic<uint64_t> counter{0};
    return std::filesystem::temp_directory_path() /
           std::format("{}_{}_{}", prefix, static_cast<unsigned long long>(::getpid()), counter.fetch_add(1));
}

void IncrementCleanup(void* arg1, void* /*arg2*/) { (*reinterpret_cast<int*>(arg1))++; }

class CleanupTrackingIterator : public Iterator {
   public:
    bool Valid() const override { return false; }
    void SeekToFirst() override {}
    void SeekToLast() override {}
    void Seek(const std::string_view& /*target*/) override {}
    void Next() override { FAIL() << "unexpected Next"; }
    void Prev() override { FAIL() << "unexpected Prev"; }
    std::string_view key() const override { return {}; }
    std::string_view value() const override { return {}; }
    Status status() const override { return Status::OK(); }
};

}  // namespace

TEST(ResourceCleanupTest, RegisteredCleanupRunsOnDelete) {
    int cleanup_calls = 0;
    Iterator* iter = new CleanupTrackingIterator();
    iter->RegisterCleanup(&IncrementCleanup, &cleanup_calls, nullptr);
    iter->RegisterCleanup(&IncrementCleanup, &cleanup_calls, nullptr);

    delete iter;

    EXPECT_EQ(cleanup_calls, 2);
}

TEST(ResourceCleanupTest, DeletingIteratorReleasesPinnedBlockCacheEntries) {
    InitGlobals();

    const std::filesystem::path dbname = UniqueTestDir("deltadb_cleanup");
    std::filesystem::create_directories(dbname);

    {
        SSTCache sst_cache(dbname.string(), 16);
        const auto* internal_comparator = static_cast<const InternalKeyComparator*>(gDBConfig->internal_comparator);
        ASSERT_NE(internal_comparator, nullptr);

        MemTable* mem = new MemTable(*internal_comparator);
        mem->Ref();
        mem->Add(1, kTypeValue, "alpha", "one");
        mem->Add(2, kTypeValue, "beta", "two");
        mem->Add(3, kTypeValue, "gamma", "three");

        SSTMetaData meta;
        meta.sst_number = 1;

        Iterator* build_iter = mem->NewIterator();
        Status build_status = BuildSST(dbname.string(), &sst_cache, build_iter, &meta);
        delete build_iter;
        mem->Unref();

        ASSERT_TRUE(build_status.ok()) << build_status.ToString();
        ASSERT_GT(meta.sst_size, 0U);

        ReadOptions options;
        options.fill_cache = true;
        Iterator* iter = sst_cache.NewIterator(options, meta.sst_number, meta.sst_size);
        ASSERT_TRUE(iter->status().ok()) << iter->status().ToString();

        iter->SeekToFirst();
        ASSERT_TRUE(iter->Valid());

        const size_t pinned_charge = gDBConfig->block_cache->TotalCharge();
        ASSERT_GT(pinned_charge, 0U);

        gDBConfig->block_cache->Prune();
        EXPECT_EQ(gDBConfig->block_cache->TotalCharge(), pinned_charge);

        delete iter;

        gDBConfig->block_cache->Prune();
        EXPECT_EQ(gDBConfig->block_cache->TotalCharge(), 0U);
    }

    std::filesystem::remove_all(dbname);
}
