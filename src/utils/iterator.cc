#include <deltadb/utils/iterator.h>

namespace delta {

Iterator::Iterator() = default;

Iterator::~Iterator() {
    CleanupNode* node = &cleanup_head_;
    while (node != nullptr && !node->IsEmpty()) {
        CleanupNode* next = node->next;
        node->Run();
        if (node != &cleanup_head_) {
            delete node;
        }
        node = next;
    }
}

void Iterator::RegisterCleanup(CleanupFunction func, void* arg1, void* arg2) {
    assert(func != nullptr);
    CleanupNode* node;
    if (cleanup_head_.IsEmpty()) {
        node = &cleanup_head_;
    } else {
        node = new CleanupNode();
        node->next = cleanup_head_.next;
        cleanup_head_.next = node;
    }
    node->function = func;
    node->arg1 = arg1;
    node->arg2 = arg2;
}

namespace {

class EmptyIterator : public Iterator {
   public:
    EmptyIterator(const Status& s) : status_(s) {}
    ~EmptyIterator() override = default;

    bool Valid() const override { return false; }
    void Seek(const std::string_view& target) override { (void)target; }
    void SeekToFirst() override {}
    void SeekToLast() override {}
    void Next() override { assert(false); }
    void Prev() override { assert(false); }
    std::string_view key() const override {
        assert(false);
        return std::string_view();
    }
    std::string_view value() const override {
        assert(false);
        return std::string_view();
    }
    Status status() const override { return status_; }

   private:
    Status status_;
};

}  // anonymous namespace

Iterator* NewEmptyIterator() { return new EmptyIterator(Status::OK()); }

Iterator* NewErrorIterator(const Status& status) { return new EmptyIterator(status); }

}  // namespace delta
