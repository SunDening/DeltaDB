#include "compaction_manager.h"

CompactionManager::CompactionManager() {}

CompactionManager::~CompactionManager() { stop(); }

void CompactionManager::start() {
    stop_flag = false;
    worker = std::thread(&CompactionManager::run, this);
}

void CompactionManager::stop() {
    stop_flag = true;
    queue_cv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

void CompactionManager::enqueue_task(const CompactionTask &task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        task_queue.push(task);
    }
    queue_cv.notify_one();
}

void CompactionManager::set_compact_executor(std::function<void(CompactionTask)> exec) { executor = exec; }

void CompactionManager::run() {
    while (!stop_flag) {
        CompactionTask task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [this]() { return !task_queue.empty() || stop_flag; });
            if (stop_flag) break;

            task = task_queue.front();
            task_queue.pop();
        }

        if (executor) {
            executor(task);
        }
    }
}
