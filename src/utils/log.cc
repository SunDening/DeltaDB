#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>

#include <deltadb/utils/log.h>

namespace delta {

extern delta::Config::ptr gDBConfig;
extern delta::Logger::ptr gDBLogger;

static std::atomic_int64_t g_db_log_index{0};

void CoredumpHandler(int signal_no) {
    static constexpr char kMessage[] = "progress received invalid signal, will exit\n";
    [[maybe_unused]] const ssize_t ignored = ::write(STDERR_FILENO, kMessage, sizeof(kMessage) - 1);

    signal(signal_no, SIG_DFL);
    raise(signal_no);
}

static thread_local pid_t t_thread_id = 0;
static pid_t g_pid = 0;

// LogLevel g_log_level = DEBUG;

pid_t gettid() {
    if (t_thread_id == 0) {
        t_thread_id = syscall(SYS_gettid);
    }
    return t_thread_id;
}

bool OpenLog() {
    if (!gDBLogger) {
        return false;
    }
    return true;
}

LogEvent::LogEvent(LogLevel level, const char *file_name, int line, const char *func_name, LogType type)
    : level_(level), file_name_(file_name), line_(line), func_name_(func_name), type_(type) {}

LogEvent::~LogEvent() {}

std::stringstream &LogEvent::getStringStream() {
    gettimeofday(&timeval_, nullptr);

    struct tm time;
    localtime_r(&(timeval_.tv_sec), &time);

    const char *format = "%Y-%m-%d %H:%M:%S";
    char buf[128];
    strftime(buf, sizeof(buf), format, &time);

    ss_ << "[" << buf << "." << timeval_.tv_usec << "]\t";

    std::string s_level = levelToString(level_);
    ss_ << "[" << s_level << "]\t";

    if (g_pid == 0) {
        g_pid = getpid();
    }
    pid_ = g_pid;

    if (t_thread_id == 0) {
        t_thread_id = gettid();
    }
    tid_ = t_thread_id;

    ss_ << "[" << pid_ << "]\t"
        << "[" << tid_ << "]\t"
        << "[" << file_name_ << ":" << line_ << "]\t";

    return ss_;
}

std::string LogEvent::toString() { return getStringStream().str(); }

void LogEvent::log() {
    ss_ << "\n";
    if (level_ >= gDBConfig->db_log_level_ && type_ == DB_LOG) {
        gDBLogger->pushDBLog(ss_.str());
    }
}

LogTmp::LogTmp(LogEvent::ptr event) : event_(event) {}

std::stringstream &LogTmp::getStringStream() { return event_->getStringStream(); }

LogTmp::~LogTmp() { event_->log(); }

Logger::Logger() {
    // cannot do anything which will call LOG ,otherwise is will coredump
    init(gDBConfig->log_prefix_.c_str(), gDBConfig->log_path_.c_str(), gDBConfig->log_max_file_size_,
         gDBConfig->log_sync_interval_);
    std::cout << "-------- init success --------" << std::endl;
}

Logger::~Logger() {
    if (is_init_) {
        stop();
        flush();
        if (async_db_logger_->thread_.joinable()) {
            async_db_logger_->thread_.join();
        }
    }
}

Logger *Logger::GetLogger() { return gDBLogger.get(); }

void Logger::init(const char *file_name, const char *file_path, int max_size, int sync_interval) {
    if (!is_init_) {
        sync_interval_ = sync_interval;
        // db_buffer.assign(1000000, std::string());
        db_buffer.reserve(1000000);

        async_db_logger_ = std::make_shared<AsyncLogger>(file_name, file_path, max_size, DB_LOG);

        signal(SIGSEGV, CoredumpHandler);
        signal(SIGABRT, CoredumpHandler);
        signal(SIGTERM, CoredumpHandler);
        signal(SIGKILL, CoredumpHandler);
        signal(SIGINT, CoredumpHandler);
        signal(SIGSTKFLT, CoredumpHandler);

        // ignore SIGPIPE
        signal(SIGPIPE, SIG_IGN);
        is_init_ = true;
    }
}

void Logger::start() {
    if (!is_thread_running_) {
        is_thread_running_ = true;
        t_ = std::thread(&Logger::logThreadFunc, this);
    }
}

void Logger::stop() {
    is_thread_running_ = false;
    if (t_.joinable()) {
        t_.join();
    }
}

void Logger::logThreadFunc() {
    while (is_thread_running_) {
        loopFunc();
        std::this_thread::sleep_for(std::chrono::milliseconds(sync_interval_));
    }
}

void Logger::loopFunc() {
    std::vector<std::string> tmp;
    {
        std::lock_guard<std::mutex> lock(db_buff_mtx_);
        tmp.swap(db_buffer);
    }
    async_db_logger_->push(tmp);
}

void Logger::pushDBLog(const std::string &msg) {
    {
        std::lock_guard<std::mutex> lock(db_buff_mtx_);
        db_buffer.push_back(std::move(msg));
    }
}

void Logger::flush() {
    loopFunc();
    async_db_logger_->stop();
    async_db_logger_->flush();
}

AsyncLogger::AsyncLogger(std::string file_name, std::string file_path, int max_size, LogType logtype)
    : file_name_(file_name), file_path_(file_path), max_size_(max_size), log_type_(logtype) {
    int rt = sem_init(&semaphore_, 0, 0);
    assert(rt == 0);
    (void)rt;  // 告诉编译器我知道它没被用，别报警了

    thread_ = std::thread(&AsyncLogger::execute, this);
    rt = sem_wait(&semaphore_);
    assert(rt == 0);
}

AsyncLogger::~AsyncLogger() {
    stop();
    cv_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
    sem_destroy(&semaphore_);
}

void *AsyncLogger::execute(void *arg) {
    AsyncLogger *ptr = reinterpret_cast<AsyncLogger *>(arg);

    int rt = sem_post(&ptr->semaphore_);
    assert(rt == 0);
    (void)rt;  // 告诉编译器我知道它没被用，别报警了

    while (1) {
        std::vector<std::string> tmp;
        bool is_stop;
        {
            std::unique_lock<std::mutex> lock(ptr->mtx_);
            ptr->cv_.wait(lock, [ptr] { return !ptr->tasks.empty() || ptr->stop_; });

            if (!ptr->tasks.empty()) {
                tmp.swap(ptr->tasks.front());
                ptr->tasks.pop();
            }
            is_stop = ptr->stop_;
        }

        timeval now;
        gettimeofday(&now, nullptr);

        struct tm now_time;
        localtime_r(&(now.tv_sec), &now_time);

        const char *format = "%Y%m%d";
        char date[32];
        strftime(date, sizeof(date), format, &now_time);
        if (ptr->date_ != std::string(date)) {
            ptr->no_ = 0;
            ptr->date_ = std::string(date);
            ptr->need_reopen_ = true;
        }

        if (!ptr->file_handle_) {
            ptr->need_reopen_ = true;
        }

        // 确保日志所在目录存在
        namespace fs = std::filesystem;

        // 检查路径是否存在，不存在则创建（包括多级目录）
        if (!fs::exists(ptr->file_path_)) {
            try {
                fs::create_directories(ptr->file_path_);  // 递归创建所有缺失的父目录
            } catch (const fs::filesystem_error &e) {
                // 可选：记录错误或抛出异常
                throw std::runtime_error(std::format("日志目录 {} 不存在. ", ptr->file_path_) + std::string(e.what()));
            }
        }

        std::string full_file_name = std::format("{}{}_{}_{}_{}.log", ptr->file_path_, ptr->file_name_, ptr->date_,
                                                 LogTypeToString(ptr->log_type_), ptr->no_);

        if (ptr->need_reopen_) {
            if (ptr->file_handle_) {
                fclose(ptr->file_handle_);
            }

            ptr->file_handle_ = fopen(full_file_name.c_str(), "a");
            if (ptr->file_handle_ == nullptr) {
                printf("open fail errno = %d reason = %s \n", errno, strerror(errno));
            }
            ptr->need_reopen_ = false;
        }

        if (ptr->file_handle_ && ftell(ptr->file_handle_) > ptr->max_size_) {
            fclose(ptr->file_handle_);

            // single log file over max size
            ptr->no_++;
            full_file_name = std::format("{}{}_{}_{}_{}.log", ptr->file_path_, ptr->file_name_, ptr->date_,
                                         LogTypeToString(ptr->log_type_), ptr->no_);

            ptr->file_handle_ = fopen(full_file_name.c_str(), "a");
            ptr->need_reopen_ = false;
        }

        if (!ptr->file_handle_) {
            printf("open log file %s error!", full_file_name.c_str());
        }

        for (auto i : tmp) {
            if (!i.empty()) {
                fwrite(i.c_str(), 1, i.length(), ptr->file_handle_);
            }
        }
        tmp.clear();
        fflush(ptr->file_handle_);
        if (is_stop) {
            break;
        }
    }
    if (ptr->file_handle_) {
        fclose(ptr->file_handle_);
    }

    return nullptr;
}

void AsyncLogger::push(std::vector<std::string> &buffer) {
    if (!buffer.empty()) {
        {
            std::unique_lock<std::mutex> lock(mtx_);
            tasks.push(buffer);
        }
        cv_.notify_one();
    }
}

void AsyncLogger::flush() {
    if (file_handle_) {
        fflush(file_handle_);
    }
}

void AsyncLogger::stop() {
    if (!stop_) {
        stop_ = true;
        cv_.notify_one();
    }
}

void Exit(int code) {
    std::cout << "It's sorry to said we start delta error, look up log file to get more details!" << std::endl;

    gDBLogger->flush();

    if (gDBLogger->getAsyncDBLogger()->thread_.joinable()) {
        gDBLogger->getAsyncDBLogger()->thread_.join();
    }

    _exit(code);
}

}  // namespace delta
