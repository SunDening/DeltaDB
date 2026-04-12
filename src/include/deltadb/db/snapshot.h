#pragma once

#include <deltadb/db/db.h>
#include <deltadb/utils/dbformat.h>

namespace delta {

/**
 * 快照（Snapshot）提供数据库的一致性读视图，确保读取操作看到的数据在快照创建后保持不变
 *
 * 快照自身不存储数据，只维护一个序列号。快照只是一个"时间点标记"
 * 读数据时传入快照参数，只能读取 ≤ 该快照序列号的数据
 *
时间线：
─────────────────────────────────────────────────────────────→
       Put("k1", "v1")    Put("k1", "v2")    Put("k1", "v3")
           @seq=50            @seq=100           @seq=150
                                  ↑
                              快照创建点
                              snapshot@100

读取结果：
┌──────────────────┬──────────────────────────────────────────┐
│  读操作           │  返回值                                 │
├──────────────────┼──────────────────────────────────────────┤
│  Get("k1")       │  "v3"  (最新版本)                        │
│  (无快照)        │                                          │
├──────────────────┼──────────────────────────────────────────┤
│  Get("k1", snap) │  "v2"  (snapshot@100 能看到的最新版本)    │
│  (有快照)        │  因为 v3@150 > 100，不可见 ?             │
│                  │  v2@100 <= 100，可见 ?                    │
└──────────────────┴──────────────────────────────────────────┘
 *
 *  Snapshot (公共 API，抽象基类) (在 db.h 中)
        ↑
        │ 继承
        │
    SnapshotImpl (内部实现，带序列号和链表指针)
        │
        │ 被管理于
        ↓
    SnapshotList (双向循环链表管理器)
 *
 */

class SnapshotList;

class SnapshotImpl : public Snapshot {
   private:
    friend class SnapshotList;

    // 双向循环链表指针
    SnapshotImpl* prev_;
    SnapshotImpl* next_;

    // 快照对应的序列号（快照的时间点标识）
    // 快照创建后，序列号不可修改（const保证）
    const SequenceNumber sequence_number_;

   public:
    SnapshotImpl(SequenceNumber sequence_number) : sequence_number_(sequence_number) {
        InfoLog << std::format("SnapshotImpl init, sequence_number_: {}", sequence_number_);
    }

    SequenceNumber sequence_number() const { return sequence_number_; }
};

/**
 * 双向循环链表
 * 布局：
         head_
        +-------+
        | dummy |
        +-------+
       ↗       ↖
  newest()   oldest()
    ↓           ↓
  +---+       +---+
  | S1| ←──→ | S2|
  +---+       +---+
     ↖       ↗
      (循环)
 *
 */
class SnapshotList {
   private:
    // 哑元头节点(dummy head)（简化边界处理）
    SnapshotImpl head_;

   public:
    SnapshotList() : head_(0) {
        head_.prev_ = &head_;
        head_.next_ = &head_;
    }

    ~SnapshotList() {
        while (!empty()) {
            Delete(oldest());
        }
    }

    bool empty() const { return head_.next_ == &head_; }

    // 第一个真实节点
    SnapshotImpl* oldest() const {
        assert(!empty());
        return head_.next_;
    }

    // 最后一个真实节点
    SnapshotImpl* newest() const {
        assert(!empty());
        return head_.prev_;
    }

    // 创建快照（尾插法）
    SnapshotImpl* New(SequenceNumber seq_num) {
        // 序列号必须递增（快照按时间顺序创建）
        assert(empty() || newest()->sequence_number_ <= seq_num);

        SnapshotImpl* snapshot = new SnapshotImpl(seq_num);

        snapshot->next_ = &head_;
        snapshot->prev_ = head_.prev_;
        snapshot->prev_->next_ = snapshot;
        snapshot->next_->prev_ = snapshot;
        return snapshot;
    }

    void Delete(const SnapshotImpl* snapshot) {
        // 标准双向链表删除操作
        snapshot->prev_->next_ = snapshot->next_;
        snapshot->next_->prev_ = snapshot->prev_;
        delete snapshot;
    }
};

}  // namespace delta
