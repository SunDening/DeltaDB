#pragma once

#include <semaphore.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <condition_variable>
#include <format>
#include <memory>
#include <queue>
#include <sstream>
#include <thread>
#include <vector>

#include <deltadb/utils/config.h>

namespace delta {

extern delta::Config::ptr gDBConfig;

template <typename... Args>
std::string formatString(const std::string_view fmt, Args&&... args) {
    return std::vformat(fmt, std::make_format_args(args...));
}

#define DebugLog                                                                                                 \
    if (delta::OpenLog() && delta::LogLevel::DEBUG >= delta::gDBConfig->db_log_level_)                           \
    delta::LogTmp(delta::LogEvent::ptr(new delta::LogEvent(delta::LogLevel::DEBUG, __FILE__, __LINE__, __func__, \
                                                           delta::LogType::DB_LOG)))                             \
        .getStringStream()

#define InfoLog                                                                                                 \
    if (delta::OpenLog() && delta::LogLevel::INFO >= delta::gDBConfig->db_log_level_)                           \
    delta::LogTmp(delta::LogEvent::ptr(new delta::LogEvent(delta::LogLevel::INFO, __FILE__, __LINE__, __func__, \
                                                           delta::LogType::DB_LOG)))                            \
        .getStringStream()

#define WarnLog                                                                                                 \
    if (delta::OpenLog() && delta::LogLevel::WARN >= delta::gDBConfig->db_log_level_)                           \
    delta::LogTmp(delta::LogEvent::ptr(new delta::LogEvent(delta::LogLevel::WARN, __FILE__, __LINE__, __func__, \
                                                           delta::LogType::DB_LOG)))                            \
        .getStringStream()

#define ErrorLog                                                                                                 \
    if (delta::OpenLog() && delta::LogLevel::ERROR >= delta::gDBConfig->db_log_level_)                           \
    delta::LogTmp(delta::LogEvent::ptr(new delta::LogEvent(delta::LogLevel::ERROR, __FILE__, __LINE__, __func__, \
                                                           delta::LogType::DB_LOG)))                             \
        .getStringStream()

pid_t gettid();

bool OpenLog();

class LogEvent {
   public:
    typedef std::shared_ptr<LogEvent> ptr;
    LogEvent(LogLevel level, const char* file_name, int line, const char* func_name, LogType type);

    ~LogEvent();

    std::stringstream& getStringStream();

    std::string toString();

    void log();

   private:
    // uint64_t m_timestamp;
    timeval timeval_;  // 时间间隔
    LogLevel level_;   // 日志级别
    pid_t pid_{0};
    pid_t tid_{0};

    const char* file_name_;  // 文件名(源文件，不是日志文件)
    int line_{0};            // 行号
    const char* func_name_;  // 函数名
    LogType type_;           // 日志类型
    std::string msg_no_;     // 日志编号

    std::stringstream ss_;  // 日志内容流
};

class LogTmp {
   public:
    explicit LogTmp(LogEvent::ptr event);

    ~LogTmp();

    std::stringstream& getStringStream();

   private:
    LogEvent::ptr event_;
};

class AsyncLogger {
   public:
    typedef std::shared_ptr<AsyncLogger> ptr;

    AsyncLogger(std::string file_name, std::string file_path, int max_size, LogType logtype);
    ~AsyncLogger();

    void push(std::vector<std::string>& buffer);

    void flush();

    static void* execute(void*);

    void stop();

   public:
    std::queue<std::vector<std::string>> tasks;

   private:
    std::string file_name_;
    std::string file_path_;
    int max_size_{0};
    LogType log_type_;
    int no_{0};
    bool need_reopen_{false};
    FILE* file_handle_{nullptr};
    std::string date_;

    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_{false};

   public:
    std::thread thread_;
    sem_t semaphore_;
};

class Logger {
   public:
    typedef std::shared_ptr<Logger> ptr;

    std::vector<std::string> db_buffer;  // DB日志缓冲区

    static Logger* GetLogger();

    Logger();
    ~Logger();

    void init(const char* file_name, const char* file_path, int max_size, int sync_interval);

    void pushDBLog(const std::string& log_msg);
    void logThreadFunc();
    void loopFunc();

    void flush();

    void start();
    void stop();  // 新增：停止日志线程

    AsyncLogger::ptr getAsyncDBLogger() { return async_db_logger_; }

   private:
    std::mutex db_buff_mtx_;
    bool is_init_{false};
    AsyncLogger::ptr async_db_logger_;

    int sync_interval_{0};

    // 新增
    std::thread t_;                  // 日志线程
    bool is_thread_running_{false};  // 线程运行标志
};

void Exit(int code);

}  // namespace delta
