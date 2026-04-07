#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace delta {
/**
 * @brief 一个单线程线程池。
 * 核心特征：
 *  1. 只有一个工作线程 — std::thread thread_;（第72行）
 *  2. 首次提交时启动 — 第一次调用 Schedule() 时才创建线程
 *  3. 串行执行任务 — BackgroundThreadMain() 循环从队列中取任务，一个一个执行
 */
class SingleThreadThreadPool {
   public:
    SingleThreadThreadPool() : stop_(false) {}

    ~SingleThreadThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            cv_.notify_one();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // 提交任务到线程池
    void Schedule(void (*function)(void* arg), void* arg) {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // 启动后台线程（如果还未启动）
            if (!thread_.joinable()) {
                thread_ = std::thread(&SingleThreadThreadPool::BackgroundThreadMain, this);
            }

            // 入队
            queue_.emplace(function, arg);
        }
        // 通知后台线程有新任务
        cv_.notify_one();
    }

   private:
    void BackgroundThreadMain() {
        while (true) {
            WorkItem item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !queue_.empty() || stop_; });

                if (stop_ && queue_.empty()) {
                    return;
                }

                item = queue_.front();
                queue_.pop();
            }

            // 执行任务（不持有锁）
            item.function(item.arg);
        }
    }

    struct WorkItem {
        void (*function)(void*);
        void* arg;

        WorkItem() : function(nullptr), arg(nullptr) {}
        WorkItem(void (*func)(void*), void* a) : function(func), arg(a) {}
    };

    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_;
    std::thread thread_;
    std::queue<WorkItem> queue_;
};
}  // namespace delta
