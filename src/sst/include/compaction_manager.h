#pragma once

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

struct CompactionTask {
    int target_level;
};

class CompactionManager {
public:
    CompactionManager();
    ~CompactionManager();

    void start();
    void stop();

    // compact任务入队
    void enqueue_task(const CompactionTask& task);

    // 注册合并任务执行逻辑（这里就是调用compact）
    void set_compact_executor(std::function<void(CompactionTask)> executor);

private:
    std::queue<CompactionTask> task_queue;  // 任务队列
    std::mutex queue_mutex;
    std::condition_variable queue_cv;  // 条件变量
    std::thread worker;  // compact工作线程
    std::atomic<bool> stop_flag{false};

    std::function<void(CompactionTask)> executor;

    // 循环逻辑
    void run();
};
