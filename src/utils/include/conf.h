#pragma once

#include <tinyxml.h>
#include <format>
#include <map>
#include <memory>
#include <string>

namespace delta {

enum LogLevel { DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, NONE = 5 };

class Config {
   private:
    std::string conf_file_path_;
    TiXmlDocument *xml_file_;  // xml文件

   public:
    typedef std::shared_ptr<Config> ptr;

    // 日志参数 (log params)
    std::string log_path;       // 日志路径
    std::string log_base_name;  // 日志基础名称
    int log_max_file_size;      // 单日志文件最大容量
    int log_max_file_num;       // 最大共存日志数目

    // 预写日志参数（wal params）
    std::string wal_path;       // 预写日志路径
    std::string wal_base_name;  // 预写日志基础名称
    int wal_max_file_size;      // 单预写日志文件最大容量

    Config(const std::string &conf_file_path);

    ~Config();

    void readConf();

    void readLogConfig(TiXmlElement *node);

    void readWalConfig(TiXmlElement *node);

    TiXmlElement *getXmlNode(const std::string &name);

    void checkConf(TiXmlElement *node, std::string conf_option);
};

}  // namespace delta